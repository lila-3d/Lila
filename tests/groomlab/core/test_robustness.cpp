// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Stability: no crash and no stray exception on bad input. Operators may only
// throw GraphError (shown on the node); strand outputs must stay valid.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "groomlab/expression.h"
#include "groomlab/graph_io.h"
#include "groomlab/ramp.h"
#include "groomlab/groom.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operators.h"
#include "test_helpers.h"
#include "groom/GroomEvaluator.h"

using lila::groom::GroomEvaluator;

namespace groomlab {
namespace {

const float kNaN = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

std::vector<std::pair<const char*, MeshPtr>> badMeshes() {
    std::vector<std::pair<const char*, MeshPtr>> out;
    out.emplace_back("null", nullptr);
    out.emplace_back("empty", std::make_shared<ScalpMesh>());
    // Every triangle collapsed to a point or a line.
    out.emplace_back("degenerate", std::make_shared<ScalpMesh>(
                                       std::vector<Vec3f>{{0, 0, 0}, {0, 0, 0}, {1, 0, 0}, {2, 0, 0}},
                                       std::vector<ScalpMesh::Triangle>{{0, 1, 2}, {1, 2, 3}, {0, 0, 0}}));
    auto nan = std::make_shared<ScalpMesh>(*test::makePlane(3));
    std::vector<Vec3f> pts = nan->points();
    pts[2] = {kNaN, 0, 0};
    pts[5] = {kInf, kInf, 0};
    out.emplace_back("nonfinite", std::make_shared<ScalpMesh>(pts, nan->triangles()));
    out.emplace_back("huge", std::make_shared<ScalpMesh>(std::vector<Vec3f>{{0, 0, 0}, {1e30f, 0, 0}, {0, 0, 1e30f}},
                                                         std::vector<ScalpMesh::Triangle>{{0, 1, 2}}));
    return out;
}

std::vector<std::pair<const char*, StrandsPtr>> badStrands() {
    std::vector<std::pair<const char*, StrandsPtr>> out;
    out.emplace_back("null", nullptr);
    out.emplace_back("empty", std::make_shared<StrandSet>());
    auto odd = std::make_shared<StrandSet>();
    odd->addStrand(1, {{0, 0, 0}});                                   // one point
    odd->addStrand(2, {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}});             // zero length
    odd->addStrand(3, {{0, 0, 0}, {kNaN, 0, 0}, {0, 1, 0}});          // non-finite
    odd->addStrand(4, {{0, 0, 0}, {0, 1, 0}}, {}, StrandRoot{9999, 0.2f, 0.2f});  // root off the mesh
    out.emplace_back("odd", odd);
    return out;
}

std::vector<std::pair<const char*, GuidesPtr>> badGuides() {
    std::vector<std::pair<const char*, GuidesPtr>> out;
    out.emplace_back("null", nullptr);
    out.emplace_back("empty", std::make_shared<GuideSet>());
    auto odd = std::make_shared<GuideSet>();
    odd->add(1, {{0, 0, 0}});
    odd->add(2, {{0, 0, 0}, {0, 0, 0}});
    odd->add(3, {{kNaN, 0, 0}, {0, 1, 0}});
    out.emplace_back("odd", odd);
    return out;
}

Value inputFor(PortType type, std::size_t variant, const std::vector<std::pair<const char*, MeshPtr>>& meshes,
               const std::vector<std::pair<const char*, StrandsPtr>>& strands,
               const std::vector<std::pair<const char*, GuidesPtr>>& guides) {
    switch (type) {
    case PortType::Strands:
        return strands[variant % strands.size()].second;
    case PortType::Guides:
        return guides[variant % guides.size()].second;
    case PortType::Mesh:
    default:
        return meshes[variant % meshes.size()].second;
    }
}

TEST(Robustness, OperatorsSurviveBadInputs) {
    const auto meshes = badMeshes();
    const auto strands = badStrands();
    const auto guides = badGuides();
    for (const std::string& type : OperatorRegistry::instance().typeNames()) {
        for (std::size_t variant = 0; variant < 5; ++variant) {
            for (const auto& [meshName, scalp] : meshes) {
                auto op = OperatorRegistry::instance().create(type);
                if (type == op::kScatter) op->params().set("density", 50.0);
                ExternalInputs ext;
                if (scalp) ext.set("scalp", scalp, scalp->contentHash());
                if (guides[variant % guides.size()].second) {
                    ext.set("guides", guides[variant % guides.size()].second, variant);
                }
                EvalContext ctx;
                ctx.externals = &ext;
                std::vector<Value> inputs;
                for (const PortSpec& port : op->inputs()) inputs.push_back(inputFor(port.type, variant, meshes, strands, guides));
                SCOPED_TRACE(type + " variant " + std::to_string(variant) + " mesh " + meshName);
                try {
                    const Value out = op->evaluate(ctx, inputs);
                    if (const StrandsPtr s = valueAs<StrandsPtr>(out)) {
                        std::string error;
                        EXPECT_TRUE(s->validate(&error)) << error;
                    }
                } catch (const GraphError&) {
                    // Expected for missing inputs: shown on the node.
                } catch (const std::exception& e) {
                    ADD_FAILURE() << "non-GraphError exception: " << e.what();
                }
            }
        }
    }
}

TEST(Robustness, MissingMapsAreGraphErrors) {
    const MeshPtr plane = test::makePlane(2);
    ExternalInputs ext;
    ext.set("scalp", plane, plane->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;
    auto scatter = OperatorRegistry::instance().create(op::kScatter);
    scatter->params().set("density", ParamValue(MapRef{MapRef::Source::Texture, "no/such/texture.png"}));
    EXPECT_THROW(scatter->evaluate(ctx, {plane}), GraphError);  // plane has no UVs, or file is missing
    scatter->params().set("density", ParamValue(MapRef{MapRef::Source::ColorSet, "missing"}));
    EXPECT_THROW(scatter->evaluate(ctx, {plane}), GraphError);
}

TEST(Robustness, GroomsSurviveBadData) {
    for (const auto& [name, scalp] : badMeshes()) {
        SCOPED_TRACE(name);
        GroomData d;
        d.scalp = scalp;
        d.hierarchyJson = R"({"version": 1, "groups": [], "guides": []})";
        EXPECT_NO_THROW(GroomEvaluator(d).evaluate(1.0f, 0.0));
    }
    GroomData bad;
    bad.graphJson = "{not json";
    EXPECT_THROW(GroomEvaluator{bad}, GraphError);
    bad.graphJson = R"({"version": 99})";
    EXPECT_THROW(GroomEvaluator{bad}, GraphError);  // newer schema: a clear error
    bad.graphJson = "";
    bad.hierarchyJson = R"({"version": 1, "groups": [{"id": 1}]})";
    EXPECT_THROW(GroomEvaluator{bad}, std::runtime_error);

    // A hierarchy on degenerate scalps.
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    h.addGuide(g, test::straightCurve({0, 0, 0}, {0, 1, 0}));
    h.addGuide(g, {{0, 0, 0}, {0, 0, 0}});
    ChildGenerationParams params;
    h.generateChildren(g, "children", params);
    for (const auto& [name, scalp] : badMeshes()) {
        SCOPED_TRACE(name);
        GuideHierarchy copy = h;
        EXPECT_NO_THROW(copy.evaluate(scalp.get()));
    }
}

TEST(Robustness, CurvesStayBetweenZeroAndOne) {
    ParamSet params;
    params.set("profile", Ramp{{-0.5f, 2.0f}, {0.5f, -1.0f}, {1.5f, 0.5f}});
    const Ramp r = params.getRamp("profile");
    for (const auto& [t, v] : r.keys) {
        EXPECT_GE(t, 0.0f);
        EXPECT_LE(t, 1.0f);
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
    for (int i = 0; i <= 20; ++i) {
        const float v = r(static_cast<float>(i) / 20.0f);
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
    params.set("flat", 3.0);
    EXPECT_FLOAT_EQ(params.getRamp("flat")(0.5f), 1.0f);
}

TEST(Robustness, GuardsFromTheCrashReview) {
    // A NaN along a curve reads an end key, never past the keys.
    const Ramp ramp = Ramp::linear(0.2f, 0.8f);
    EXPECT_FLOAT_EQ(ramp(kNaN), 0.2f);
    Ramp curved({{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}});
    curved.straight = false;
    EXPECT_TRUE(std::isfinite(curved(kNaN)));
    // Nesting deep enough to overflow the stack is a clear error instead.
    EXPECT_THROW(Expression(std::string(100000, '(') + "1" + std::string(100000, ')')), ExpressionError);
    EXPECT_THROW(Expression(std::string(100000, '-') + "1"), ExpressionError);
    EXPECT_NO_THROW(Expression("((u + 1) * -(-v))"));
    // Roots from another mesh read nothing rather than past the UVs.
    auto plane = std::make_shared<ScalpMesh>(*test::makePlane(2));
    plane->setCornerUVs(std::vector<ScalpMesh::UV>(plane->triangleCount() * 3, ScalpMesh::UV{0.5f, 0.5f}));
    StrandRoot far;
    far.face = static_cast<std::int32_t>(plane->triangleCount() + 50);
    const ScalpMesh::UV uv = plane->uvAt(far);
    EXPECT_EQ(uv[0], 0.0f);
    EXPECT_EQ(plane->colorAt("missing", far)[0], 0.0f);
    // A density that isn't a number stops with a message instead of scattering forever.
    Graph g;
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(4), 1);
    const NodeId scalp = g.addNode(op::kScalp), scatter = g.addNode(op::kScatter);
    g.connect(scalp, scatter, 0);
    EvalContext ctx;
    ctx.externals = &ext;
    g.node(scatter).params().set("density", static_cast<double>(kInf));
    EXPECT_THROW(g.evaluateStrands(scatter, ctx), GraphError);
}

}  // namespace
}  // namespace groomlab
