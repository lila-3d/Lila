// SPDX-License-Identifier: Apache-2.0
// The new-node template (tools/dev/new_node/example_op.cpp, built into these tests as "TestExample")
// as a real operator: it registers, its specs are well formed, it moves the strands as it says,
// deterministically, and it reads every param it declares. So the template every new node starts
// from (tools/dev/new_node.py) keeps compiling and working.

#include <gtest/gtest.h>

#include <cmath>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/graph.h"
#include "groomlab/graph_io.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
void registerExampleOperator(OperatorRegistry& registry);  // tools/dev/new_node/example_op.cpp

namespace {

struct Registered {
    Registered() { registerExampleOperator(OperatorRegistry::instance()); }
    ~Registered() { OperatorRegistry::instance().unregisterType("TestExample"); }
};

std::shared_ptr<const ScalpMesh> plane() { return test::makePlane(4, 1.0f); }

StrandsPtr strands() {
    const auto scalp = plane();
    auto out = std::make_shared<StrandSet>();
    StrandId id = 1;
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            const Vec3f root(0.06f + static_cast<float>(i) / 8.0f, 0.0f, 0.06f + static_cast<float>(j) / 8.0f);
            out->addStrand(id++, test::straightCurve(root, {0.0f, 0.5f, 0.0f}, 6), {}, scalp->closestPoint(root).root);
        }
    }
    return out;
}

StrandsPtr run(const Operator& op, const StrandsPtr& in, const MeshPtr& scalp = nullptr) {
    ExternalInputs externals;
    if (scalp) externals.set("scalp", scalp, 1);
    EvalContext ctx;
    ctx.externals = &externals;
    return valueAs<StrandsPtr>(op.evaluate(ctx, {Value(in)}));
}

TEST(ExampleNode, TheTemplateIsARealOperator) {
    Registered registered;
    const NodeInfo* info = nodeInfo("TestExample");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->category, "shape");
    EXPECT_FALSE(info->description.empty());
    bool listed = false;
    for (const nlohmann::json& s : nlohmann::json::parse(operatorSpecsJson())) {
        if (s["type"] != "TestExample") continue;
        listed = true;
        EXPECT_EQ(s["description"], info->description);
        EXPECT_EQ(s["category"], "shape");
    }
    EXPECT_TRUE(listed);

    // Its defaults: each tip 0.1 out (no scalp here: along the strand's first segment, up; the
    // profile 1 at the tip), the root where it was.
    auto op = OperatorRegistry::instance().create("TestExample");
    const StrandsPtr in = strands();
    const StrandsPtr out = run(*op, in);
    ASSERT_EQ(out->strandCount(), in->strandCount());
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const std::size_t n = out->strandPointCount(s);
        EXPECT_EQ(out->strandPoints(s)[0], in->strandPoints(s)[0]);
        EXPECT_NEAR(out->strandPoints(s)[n - 1].y - in->strandPoints(s)[n - 1].y, 0.1f, 1e-5f);
    }
    // On the scalp: out along its normal at the root (the plane's, up), as far.
    const MeshPtr scalp = plane();
    const StrandsPtr lifted = run(*op, in, scalp);
    for (std::size_t s = 0; s < lifted->strandCount(); ++s) {
        const std::size_t n = lifted->strandPointCount(s);
        const Vec3f moved = lifted->strandPoints(s)[n - 1] - in->strandPoints(s)[n - 1];
        EXPECT_NEAR((moved - scalp->normal(in->roots()[s]) * 0.1f).length(), 0.0f, 1e-5f);
    }
    // Randomness by strand ID: the same strands every run, other strands with another seed.
    op->params().set("distanceRandom", 0.8);
    const StrandsPtr a = run(*op, in), b = run(*op, in);
    EXPECT_EQ(a->positions(), b->positions());
    op->params().set("seed", std::int64_t{7});
    EXPECT_NE(run(*op, in)->positions(), a->positions());
}

TEST(ExampleNode, ReadsEveryParamItDeclares) {
    Registered registered;
    auto op = OperatorRegistry::instance().create("TestExample");
    auto log = std::make_shared<ParamReadLog>();
    op->params().setReadLog(log);
    run(*op, strands());
    op->params().setReadLog(nullptr);
    const std::set<std::string> read = log->names();
    for (const ParamSpec& p : op->paramSpecs()) EXPECT_TRUE(read.count(p.name)) << p.name;
}

}  // namespace
}  // namespace groomlab
