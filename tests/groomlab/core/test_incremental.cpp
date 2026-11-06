// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Incremental evaluation (graph.h: Operator::evaluateIncremental): a node whose inputs changed in
// some strands makes only those again, and its result is a full evaluation's, bit for bit.
// Each test checks results against a fresh graph's (no cache) by content hash, with the graph's
// own check on (Graph::setVerifyIncremental: every incremental result against a full evaluation,
// and the strands it says it left alone against its last result).

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/graph.h"
#include "groomlab/flow.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/guide_set.h"
#include "groomlab/instances.h"
#include "groomlab/groom_file.h"
#include "groomlab/paths.h"
#include "groomlab/presets.h"
#include "groomlab/map_ref.h"
#include "groomlab/operators.h"
#include "groomlab/parts.h"
#include "groomlab/random.h"
#include "groomlab/strand_set.h"
#include "groomlab/stroke.h"
#include "groomlab/view_projection.h"
#include "test_helpers.h"
#include "groom/GroomEvaluator.h"

using lila::groom::GroomEvaluator;

namespace groomlab {
namespace {

// Hands on the strands the host set as the external "strands" (every evaluation a new set).
class HeldStrandsOp : public Operator {
public:
    std::string typeName() const override { return "TestHeldStrands"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    std::vector<std::string> externalInputs() const override { return {"strands"}; }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override { return ctx.external("strands"); }
};

// Hands on its input; incrementally it says which strands differ from its last result by
// comparing them, as Interpolate compares its guides: the source of changes for these tests.
class DiffOp : public Operator {
public:
    std::string typeName() const override { return "TestDiff"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override { return inputs.at(0); }
    bool evaluateIncremental(const EvalContext&, const std::vector<Value>& inputs, const IncrementalInputs& incremental,
                             Value* out, StrandChanges* changed) const override {
        const StrandsPtr in = valueAs<StrandsPtr>(inputs.at(0)), last = valueAs<StrandsPtr>(incremental.previous);
        if (!in || !last || in->ids() != last->ids() || in->offsets() != last->offsets() ||
            in->strandChannelNames() != last->strandChannelNames() || in->pointChannelNames() != last->pointChannelNames()) {
            return false;
        }
        std::vector<std::uint32_t> list;
        for (std::size_t s = 0; s < in->strandCount(); ++s) {
            const std::size_t begin = in->strandBegin(s), n = in->strandPointCount(s);
            bool same = std::memcmp(in->positions().data() + begin, last->positions().data() + begin, n * sizeof(Vec3f)) == 0 &&
                        std::memcmp(in->widths().data() + begin, last->widths().data() + begin, n * sizeof(float)) == 0;
            for (const std::string& name : in->strandChannelNames()) {
                same = same && (*in->strandChannel(name))[s] == (*last->strandChannel(name))[s];
            }
            if (!same) list.push_back(static_cast<std::uint32_t>(s));
        }
        *out = list.empty() ? Value(last) : inputs.at(0);
        *changed = StrandChanges::listed(std::move(list));
        return true;
    }
};

// Lies: says nothing changed and hands on its last result (what verification must catch).
class StaleOp : public Operator {
public:
    std::string typeName() const override { return "TestStale"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override { return inputs.at(0); }
    bool evaluateIncremental(const EvalContext&, const std::vector<Value>&, const IncrementalInputs& incremental, Value* out,
                             StrandChanges* changed) const override {
        *out = incremental.previous;
        *changed = StrandChanges::listed({});
        return true;
    }
};

// Every incremental result checked against a full evaluation while a test runs.
struct Verified {
    Verified() { Graph::setVerifyIncremental(true); }
    ~Verified() { Graph::setVerifyIncremental(false); }
};

// `count` strands of 8 points with a per-strand "mask" channel.
std::shared_ptr<StrandSet> someStrands(std::size_t count, std::uint64_t seed) {
    auto out = std::make_shared<StrandSet>();
    Rng rng(seed);
    std::vector<Vec3f> points(8);
    out->addStrandChannel("mask", 1.0f);
    for (std::size_t i = 0; i < count; ++i) {
        const Vec3f root(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1));
        for (std::size_t k = 0; k < points.size(); ++k) {
            points[k] = root + Vec3f(0.01f * rng.uniform(-1, 1), 0.1f * static_cast<float>(k), 0.0f);
        }
        out->addStrand(static_cast<StrandId>(1000 + i), points);
    }
    std::vector<float>& m = *out->mutableStrandChannel("mask");
    for (std::size_t i = 0; i < count; ++i) m[i] = rng.uniform(0, 1);
    return out;
}

// A copy of `strands` with `dabs` strands moved (some points of each), as a brush dab moves some.
std::shared_ptr<StrandSet> dabbed(const StrandSet& strands, std::size_t dabs, Rng& rng) {
    auto out = std::make_shared<StrandSet>(strands);
    for (std::size_t d = 0; d < dabs; ++d) {
        const auto s = static_cast<std::size_t>(rng.nextFloat() * static_cast<float>(strands.strandCount())) % strands.strandCount();
        Vec3f* pts = out->strandPoints(s);
        for (std::size_t k = 1; k < out->strandPointCount(s); ++k) pts[k] += Vec3f(0.02f, 0.0f, 0.01f * static_cast<float>(k));
        if (d % 3 == 0) (*out->mutableStrandChannel("mask"))[s] = rng.nextFloat();
    }
    return out;
}

// The set's longest strand's length (each strand's segments summed in order, as an expression's
// `longest` reads it).
float longestOf(const StrandSet& strands) {
    float longest = 0.0f;
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        const Vec3f* pts = strands.strandPoints(s);
        float length = 0.0f;
        for (std::size_t k = 1; k < strands.strandPointCount(s); ++k) length += (pts[k] - pts[k - 1]).length();
        longest = std::max(longest, length);
    }
    return longest;
}

// `strands` with its longest strand longer still (its points half as far again from its root):
// the set's longest changes.
std::shared_ptr<StrandSet> longestLengthened(const StrandSet& strands) {
    auto out = std::make_shared<StrandSet>(strands);
    std::size_t most = 0;
    float longest = -1.0f;
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        const Vec3f* pts = strands.strandPoints(s);
        float length = 0.0f;
        for (std::size_t k = 1; k < strands.strandPointCount(s); ++k) length += (pts[k] - pts[k - 1]).length();
        if (length > longest) {
            longest = length;
            most = s;
        }
    }
    Vec3f* pts = out->strandPoints(most);
    for (std::size_t k = 1; k < out->strandPointCount(most); ++k) pts[k] = pts[0] + (pts[k] - pts[0]) * 1.5f;
    return out;
}

// held -> diff -> noise -> curl -> length -> width -> resample -> out, built in code: the test
// operators are not registered (every registered type must have a golden case).
struct Chain {
    Graph graph;
    std::map<std::string, NodeId> ids;
    ExternalInputs ext;
    EvalContext ctx;

    NodeId operator[](const std::string& name) const { return ids.at(name); }
    Operator& node(const std::string& name) { return graph.node(ids.at(name)); }
    StrandsPtr run(const StrandsPtr& strands) {
        ext.set("strands", strands, strands->contentHash());
        ctx.externals = &ext;
        return graph.evaluateStrands(ids.at("out"), ctx);
    }
};

using ChainEdit = std::function<void(Chain&)>;

std::unique_ptr<Chain> makeChain(const ChainEdit& edit = {}, bool stale = false) {
    auto chain = std::make_unique<Chain>();
    Graph& g = chain->graph;
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        chain->ids[name] = g.addNode(std::move(op));
        g.setLabel(chain->ids[name], name);
    };
    auto& registry = OperatorRegistry::instance();
    add("held", std::make_unique<HeldStrandsOp>());
    add("diff", stale ? std::unique_ptr<Operator>(std::make_unique<StaleOp>()) : std::make_unique<DiffOp>());
    add("noise", registry.create(op::kNoise));
    add("curl", registry.create(op::kCurl));
    add("length", registry.create(op::kLength));
    add("width", registry.create(op::kWidth));
    add("resample", registry.create(op::kResample));
    add("out", registry.create(op::kOutput));
    chain->node("noise").params().set("amplitude", 0.05);
    chain->node("noise").params().set("frequency", 3.0);
    chain->node("curl").params().set("radius", 0.02);
    chain->node("length").params().set("keep", 0.8);
    chain->node("length").params().set("randomness", 0.3);
    chain->node("resample").params().set("segments", std::int64_t{7});
    chain->node("resample").params().set("mode", std::string("even"));
    const char* order[] = {"held", "diff", "noise", "curl", "length", "width", "resample", "out"};
    for (std::size_t k = 0; k + 1 < std::size(order); ++k) g.connect(chain->ids[order[k]], chain->ids[order[k + 1]], 0);
    if (edit) edit(*chain);
    return chain;
}

// The output of a fresh chain (nothing cached, nothing kept) on these strands.
std::uint64_t freshHash(const StrandsPtr& strands, const ChainEdit& edit = {}) {
    return makeChain(edit)->run(strands)->contentHash();
}

TEST(Incremental, PerStrandOperatorsMakeOnlyTheChangedStrandsAgain) {
    const Verified verified;
    auto chain = makeChain();
    StrandsPtr strands = someStrands(400, 1);
    ASSERT_EQ(chain->run(strands)->contentHash(), freshHash(strands));
    Rng rng(5);
    for (int dab = 0; dab < 6; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 1 + static_cast<std::size_t>(dab) * 7, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands)) << "dab " << dab;
        // TestDiff, Noise, Curl, Length and Width made only the changed strands again.
        EXPECT_EQ(chain->graph.incrementalCount() - before, 5u) << "dab " << dab;
    }
}

TEST(Incremental, ASerialFromAnotherDocumentNamesNothingHere) {
    // Two documents of the same graph (one parsed again): a serial held from the first is not a
    // result of the second, so its changes since that serial are unknown, never "none".
    auto first = makeChain();
    auto second = makeChain();
    StrandsPtr strands = someStrands(50, 2);
    first->run(strands);
    second->run(strands);
    const std::uint64_t held = first->graph.resultSerial((*first)["out"]);
    ASSERT_NE(held, 0u);
    EXPECT_NE(second->graph.resultSerial((*second)["out"]), held);
    EXPECT_FALSE(second->graph.changesSince((*second)["out"], held));
}

TEST(Incremental, TheOutputSaysWhichStrandsChanged) {
    const Verified verified;
    auto chain = makeChain();
    StrandsPtr strands = someStrands(300, 2);
    chain->run(strands);
    const std::uint64_t serial = chain->graph.resultSerial((*chain)["out"]);
    auto moved = std::make_shared<StrandSet>(*strands);
    for (const std::size_t s : {7u, 42u, 299u}) moved->strandPoints(s)[3] += Vec3f(0.1f, 0.0f, 0.0f);
    EXPECT_EQ(chain->run(moved)->contentHash(), freshHash(moved));
    // Handed on by Resample (nothing to do) and Output: Width's changes, which are TestDiff's.
    const ChangesPtr changes = chain->graph.changesSince((*chain)["out"], serial);
    ASSERT_TRUE(changes);
    EXPECT_FALSE(changes->all);
    EXPECT_EQ(changes->strands, (std::vector<std::uint32_t>{7, 42, 299}));
    EXPECT_FALSE(chain->graph.changesSince((*chain)["out"], serial - 1));  // not the result before: unknown
    // Moved back: another set in, the same strands as two evaluations ago; every node made again
    // only those three, and the output is that first result's content.
    const std::size_t before = chain->graph.incrementalCount();
    EXPECT_EQ(chain->run(std::make_shared<StrandSet>(*strands))->contentHash(), freshHash(strands));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 5u);
}

TEST(Incremental, BypassedNodesAndParamEditsAndLayoutChanges) {
    const Verified verified;
    auto chain = makeChain();
    StrandsPtr strands = someStrands(300, 3);
    chain->run(strands);
    Rng rng(9);
    // Curl bypassed: what follows evaluates in full once, then dabs pass through it.
    const ChainEdit bypassed = [](Chain& c) { c.graph.setBypass(c["curl"], true); };
    bypassed(*chain);
    strands = dabbed(*strands, 5, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, bypassed));
    std::size_t before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 5, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, bypassed));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 4u);  // TestDiff, Noise, Length, Width
    chain->graph.setBypass((*chain)["curl"], false);
    // A param edit: that node and what follows evaluate in full, with the dab.
    const ChainEdit quieter = [](Chain& c) { c.node("noise").params().set("amplitude", 0.03); };
    quieter(*chain);
    before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 5, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, quieter));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 1u);  // TestDiff alone
    // Fewer strands: everything in full.
    auto fewer = std::make_shared<StrandSet>(strands->subset({0, 1, 2, 5, 8, 13}));
    before = chain->graph.incrementalCount();
    EXPECT_EQ(chain->run(fewer)->contentHash(), freshHash(fewer, quieter));
    EXPECT_EQ(chain->graph.incrementalCount(), before);
    // Off: nothing incremental, the same results.
    Graph::setIncrementalEvaluation(false);
    strands = dabbed(*fewer, 2, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, quieter));
    EXPECT_EQ(chain->graph.incrementalCount(), before);
    Graph::setIncrementalEvaluation(true);
}

TEST(Incremental, AnotherSetOfInstancesIsSaidWithTheStrandsThatChanged) {
    // Two results of the same strands, one strand moved and the instances made again (an Instance
    // node's probability reading a length a dab changed): the strand that moved, and that the
    // instances are another set (the nodes after evaluate in full; a host draws them anew).
    const StrandsPtr before = someStrands(50, 3);
    auto withInstances = [](const StrandSet& strands, std::size_t count) {
        auto set = std::make_shared<InstanceSet>();
        set->instances.resize(count);
        for (std::size_t k = 0; k < count; ++k) set->instances[k].strand = strands.ids()[k];
        auto out = std::make_shared<StrandSet>(strands);
        out->setInstances(set);
        return out;
    };
    const StrandsPtr a = withInstances(*before, 4);
    auto moved = withInstances(*before, 5);
    moved->strandPoints(7)[1] += Vec3f(0.0f, 0.1f, 0.0f);
    const ChangesPtr changes = strandChangesBetween(a, moved);
    ASSERT_TRUE(changes);
    EXPECT_FALSE(changes->all);
    EXPECT_EQ(changes->strands, std::vector<std::uint32_t>{7});
    EXPECT_TRUE(changes->instancesChanged);
    // The same set of instances: not said.
    auto same = std::make_shared<StrandSet>(*a);
    same->strandPoints(9)[1] += Vec3f(0.0f, 0.1f, 0.0f);
    const ChangesPtr kept = strandChangesBetween(a, same);
    ASSERT_TRUE(kept);
    EXPECT_FALSE(kept->instancesChanged);
}

TEST(Incremental, AWholeSetValueReadFallsBackToAFullEvaluation) {
    // Noise's amplitude from an expression of `longest` (the set's longest strand): a dab changing
    // the longest changes every strand's value, so Noise evaluates in full; one leaving it, only
    // the strands it moved (LongestKept). Width's per-strand mask stays local.
    const Verified verified;
    const ChainEdit maps = [](Chain& c) {
        MapRef longest;
        longest.source = MapRef::Source::Expression;
        longest.name = "0.05 * longest";
        c.node("noise").params().set("amplitude", longest);
        MapRef mask;
        mask.source = MapRef::Source::Attribute;
        mask.name = "mask";
        mask.max = 0.02f;
        c.node("width").params().set("width", mask);
    };
    auto chain = makeChain(maps);
    StrandsPtr strands = someStrands(200, 4);
    ASSERT_EQ(chain->run(strands)->contentHash(), freshHash(strands, maps));
    Rng rng(2);
    // TestDiff on every dab; Noise, with the longest changed, in full, then says which strands its
    // result changed in (Graph::setDiffFullResults): Curl, Length and Width follow them.
    bool fell = false, kept = false;
    for (int dab = 0; dab < 4; ++dab) {
        const float was = longestOf(*strands);
        strands = dab == 3 ? longestLengthened(*strands) : dabbed(*strands, 4, rng);
        const std::size_t before = chain->graph.incrementalCount();
        EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, maps)) << "dab " << dab;
        const bool same = longestOf(*strands) == was;
        (same ? kept : fell) = true;
        EXPECT_EQ(chain->graph.incrementalCount() - before, same ? 5u : 4u) << "dab " << dab;
    }
    EXPECT_TRUE(fell && kept);
}

// held -> diff -> collide (the scalp and a collider) -> smooth -> gravity -> displace -> flow ->
// flyaway -> attribute (a map of the dabbed mask) -> attribute (random) -> twist (around guides) ->
// wind -> restore (its reference the diff's strands, which change with the input) -> instance -> out.
std::unique_ptr<Chain> makeDeformChain(const ChainEdit& edit = {}) {
    auto chain = std::make_unique<Chain>();
    Graph& g = chain->graph;
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        chain->ids[name] = g.addNode(std::move(op));
        g.setLabel(chain->ids[name], name);
    };
    auto& registry = OperatorRegistry::instance();
    add("held", std::make_unique<HeldStrandsOp>());
    add("diff", std::make_unique<DiffOp>());
    for (const char* type : {op::kScalp, op::kColliderMesh, op::kGuides, op::kCollide, op::kSmooth, op::kGravity, op::kDisplace,
                             op::kFlow, op::kFlyaway, op::kTwist, op::kWind, op::kRestoreLength, op::kInstance, op::kOutput}) {
        add(type, registry.create(type));
    }
    add("mask", registry.create(op::kAttribute));
    add("random", registry.create(op::kAttribute));
    chain->node(op::kCollide).params().set("offset", 0.03);
    chain->node(op::kCollide).params().set("maxDepth", 0.3);
    chain->node(op::kSmooth).params().set("strength", 0.4);
    chain->node(op::kFlyaway).params().set("fraction", 0.3);
    MapRef mask;
    mask.source = MapRef::Source::Attribute;
    mask.name = "mask";
    chain->node("mask").params().set("name", std::string("painted"));
    chain->node("mask").params().set("value", mask);
    chain->node("random").params().set("name", std::string("r"));
    MapRef random;
    random.source = MapRef::Source::Expression;
    random.name = "rand";
    chain->node("random").params().set("value", random);
    chain->node(op::kTwist).params().set("turns", 0.5);
    chain->node(op::kWind).params().set("turbulence", 0.5);
    chain->node(op::kInstance).params().set("count", std::int64_t{2});
    chain->node(op::kInstance).params().set("probability", 0.4);
    const char* order[] = {"held", "diff", op::kCollide, op::kSmooth, op::kGravity, op::kDisplace, op::kFlow, op::kFlyaway,
                           "mask", "random", op::kTwist, op::kWind, op::kRestoreLength, op::kInstance, op::kOutput};
    for (std::size_t k = 0; k + 1 < std::size(order); ++k) g.connect(chain->ids[order[k]], chain->ids[order[k + 1]], 0);
    g.connect((*chain)[op::kScalp], (*chain)[op::kCollide], 1);
    g.connect((*chain)[op::kColliderMesh], (*chain)[op::kCollide], 2);
    g.connect((*chain)[op::kGuides], (*chain)[op::kTwist], 1);
    g.connect((*chain)["diff"], (*chain)[op::kRestoreLength], 1);
    chain->ids["out"] = chain->ids[op::kOutput];
    // The scalp a sphere the strands cross; the collider a smaller one above it; axes upright.
    const auto scalp = test::makeSphere(16, 32, 0.9f);
    chain->ext.set("scalp", MeshPtr(scalp), scalp->contentHash());
    std::vector<Vec3f> points = test::makeSphere(10, 20, 0.4f)->points();
    for (Vec3f& p : points) p += Vec3f(0.2f, 0.9f, 0.1f);
    const auto collider = std::make_shared<const ScalpMesh>(std::move(points), test::makeSphere(10, 20, 0.4f)->triangles());
    chain->ext.set("mesh:collider", MeshPtr(collider), collider->contentHash());
    auto axes = std::make_shared<GuideSet>();
    Rng rng(11);
    for (std::uint64_t i = 0; i < 6; ++i) {
        const Vec3f root(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1));
        axes->add(i + 1, test::straightCurve(root, Vec3f(0.1f, 0.8f, 0.0f), 5));
    }
    chain->ext.set("guides", GuidesPtr(axes), 0x6A11DE5ull);
    if (edit) edit(*chain);
    return chain;
}

std::uint64_t freshDeformHash(const StrandsPtr& strands, const ChainEdit& edit = {}) {
    return makeDeformChain(edit)->run(strands)->contentHash();
}

TEST(Incremental, CollideOnDeformedMeshesFollowsRandomDabs) {
    // The scalp and the collider deformed away from their rest poses: Collide asks the rest poses'
    // fields (RestPoseCollision) and still walks again only the strands a dab moved, every result
    // a fresh chain's.
    const ChainEdit deformed = [](Chain& c) {
        auto deform = [](const std::shared_ptr<const ScalpMesh>& rest, float bend) {
            auto rested = std::make_shared<ScalpMesh>(*rest);
            rested->setRestPoints(rest->points());
            std::vector<Vec3f> points = rest->points();
            for (Vec3f& p : points) p = Vec3f(p.x * 1.1f, p.y + bend * p.x * p.x, p.z - 0.05f);
            return MeshPtr(std::make_shared<const ScalpMesh>(rested->withPoints(std::move(points))));
        };
        const MeshPtr scalp = deform(test::makeSphere(16, 32, 0.9f), 0.1f);
        c.ext.set("scalp", scalp, scalp->contentHash());
        std::vector<Vec3f> points = test::makeSphere(10, 20, 0.4f)->points();
        for (Vec3f& p : points) p += Vec3f(0.2f, 0.9f, 0.1f);
        const MeshPtr collider =
            deform(std::make_shared<const ScalpMesh>(std::move(points), test::makeSphere(10, 20, 0.4f)->triangles()), -0.2f);
        c.ext.set("mesh:collider", collider, collider->contentHash());
    };
    const Verified verified;
    auto chain = makeDeformChain(deformed);
    StrandsPtr strands = someStrands(500, 7);
    ASSERT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, deformed));
    EXPECT_NE(chain->run(strands)->contentHash(), freshDeformHash(strands));  // the deformed meshes' result
    Rng rng(17);
    for (int dab = 0; dab < 6; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 1 + static_cast<std::size_t>(rng.nextFloat() * 40.0f), rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, deformed)) << "dab " << dab;
        EXPECT_EQ(chain->graph.incrementalCount() - before, 13u) << "dab " << dab;
    }
}

TEST(Incremental, CollideAndThePerStrandDeformersFollowRandomDabs) {
    // Collide walks again only the strands a dab moved (the scalp's and the collider's meshes the
    // same), and so do Smooth, Gravity, Displace, Flow, Flyaway, Attribute, Twist around guides,
    // Wind, RestoreLength (its reference changed in the same strands) and Instance: every result a
    // fresh chain's.
    const Verified verified;
    auto chain = makeDeformChain();
    StrandsPtr strands = someStrands(500, 7);
    ASSERT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands));
    Rng rng(13);
    for (int dab = 0; dab < 12; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 1 + static_cast<std::size_t>(rng.nextFloat() * 40.0f), rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands)) << "dab " << dab;
        // TestDiff and the 12 operators after it.
        EXPECT_EQ(chain->graph.incrementalCount() - before, 13u) << "dab " << dab;
    }
    // Another collider: Collide and what follows it evaluate in full, then dabs are incremental again.
    const ChainEdit moved = [](Chain& c) {
        std::vector<Vec3f> points = test::makeSphere(10, 20, 0.5f)->points();
        for (Vec3f& p : points) p += Vec3f(-0.3f, 0.7f, 0.0f);
        const auto collider = std::make_shared<const ScalpMesh>(std::move(points), test::makeSphere(10, 20, 0.5f)->triangles());
        c.ext.set("mesh:collider", MeshPtr(collider), collider->contentHash());
    };
    moved(*chain);
    std::size_t before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 6, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, moved));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 1u);  // TestDiff alone
    before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 6, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, moved));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 13u);
    // A Collide param edit, then a map reading the whole set on Smooth: those fall back.
    const ChainEdit deeper = [&](Chain& c) {
        moved(c);
        c.node(op::kCollide).params().set("maxDepth", 0.5);
    };
    deeper(*chain);
    before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 6, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, deeper));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 1u);
    const ChainEdit longest = [&](Chain& c) {
        deeper(c);
        MapRef map;
        map.source = MapRef::Source::Expression;
        map.name = "0.3 * longest";
        c.node(op::kSmooth).params().set("strength", map);
    };
    longest(*chain);
    for (int dab = 0; dab < 2; ++dab) {
        before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 6, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), freshDeformHash(strands, longest));
        // TestDiff and Collide; Smooth in full, the 10 after it on the strands it says changed.
        EXPECT_EQ(chain->graph.incrementalCount() - before, dab == 0 ? 2u : 12u);
    }
}

TEST(Incremental, InstancesOfAPerStrandChanceAreDrawnAgainOnlyWhereADabMoved) {
    // Instance's probability by each strand's length over the longest: a dab leaving the longest
    // draws again only the strands it moved, the others' instances spliced in from the last
    // result (the very set when none of theirs changed); one changing it, in full. Every result a
    // fresh chain's.
    const Verified verified;
    const ChainEdit byLength = [](Chain& c) {
        MapRef chance;
        chance.source = MapRef::Source::Expression;
        chance.name = "length / longest";
        c.node(op::kInstance).params().set("probability", chance);
        c.node(op::kInstance).params().set("count", std::int64_t{3});
    };
    auto chain = makeDeformChain(byLength);
    StrandsPtr strands = someStrands(500, 7);
    StrandsPtr last = chain->run(strands);
    ASSERT_EQ(last->contentHash(), freshDeformHash(strands, byLength));
    Rng rng(19);
    bool kept = false, fell = false;
    for (int dab = 0; dab < 8; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        const float was = longestOf(*last);
        strands = dab == 7 ? longestLengthened(*strands) : dabbed(*strands, 1 + static_cast<std::size_t>(rng.nextFloat() * 40.0f), rng);
        const StrandsPtr out = chain->run(strands);
        EXPECT_EQ(out->contentHash(), freshDeformHash(strands, byLength)) << "dab " << dab;
        const bool same = longestOf(*out) == was;
        (same ? kept : fell) = true;
        last = out;
        // TestDiff and the 12 operators after it; with the longest changed, Instance in full.
        EXPECT_EQ(chain->graph.incrementalCount() - before, same ? 13u : 12u) << "dab " << dab;
    }
    EXPECT_TRUE(kept && fell);
}

// Plugs `field` into param `param` of `node` (an exposed param), as a link in the editor does.
void plug(Chain& c, NodeId field, const std::string& node, const std::string& param) {
    const NodeId id = c[node];
    std::vector<std::string> exposed = c.graph.paramInputs(id);
    exposed.push_back(param);
    c.graph.setParamInputs(id, exposed);
    c.graph.connect(field, id, c.graph.node(id).inputs().size() + exposed.size() - 1);
}

NodeId addField(Chain& c, const std::string& name, const char* type) {
    c.ids[name] = c.graph.addNode(type);
    c.graph.setLabel(c.ids[name], name);
    return c.ids[name];
}

TEST(Incremental, FieldNodesPluggedIntoParamsKeepNodesIncremental) {
    // An Expression (the strands' mask), a FieldNoise, a FieldRemap of it and a FieldCombine of
    // the two read each strand alone (FieldValue::strandLocal): the nodes they feed stay incremental.
    const Verified verified;
    const ChainEdit fields = [](Chain& c) {
        const NodeId mask = addField(c, "mask", op::kExpression);
        c.graph.node(mask).params().set("expression", std::string("$mask"));
        const NodeId spots = addField(c, "spots", op::kFieldNoise);
        const NodeId remap = addField(c, "remap", op::kFieldRemap);
        c.graph.connect(spots, remap, 0);
        const NodeId both = addField(c, "both", op::kFieldCombine);
        c.graph.connect(mask, both, 0);
        c.graph.connect(remap, both, 1);
        plug(c, spots, "noise", "amplitude");
        plug(c, both, "curl", "radius");
        plug(c, mask, "width", "width");
    };
    auto chain = makeChain(fields);
    StrandsPtr strands = someStrands(300, 11);
    ASSERT_EQ(chain->run(strands)->contentHash(), freshHash(strands, fields));
    Rng rng(4);
    for (int dab = 0; dab < 4; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 3 + static_cast<std::size_t>(dab) * 5, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, fields)) << "dab " << dab;
        EXPECT_EQ(chain->graph.incrementalCount() - before, 5u) << "dab " << dab;  // TestDiff, Noise, Curl, Length, Width
    }
    // An Expression of `longest` plugged into Noise: with the longest changed Noise evaluates in
    // full, and tells the nodes after it which strands its result changed in
    // (Graph::setDiffFullResults), so they do not.
    const ChainEdit whole = [](Chain& c) {
        const NodeId expression = addField(c, "whole", op::kExpression);
        c.graph.node(expression).params().set("expression", std::string("0.05 * longest"));
        c.graph.disconnect(c["noise"], c.graph.node(c["noise"]).inputs().size());
        c.graph.connect(expression, c["noise"], c.graph.node(c["noise"]).inputs().size());
    };
    const ChainEdit longest = [&](Chain& c) {
        fields(c);
        whole(c);
    };
    whole(*chain);
    for (int dab = 0; dab < 3; ++dab) {
        const float was = longestOf(*strands);
        strands = dab == 2 ? longestLengthened(*strands) : dabbed(*strands, 4, rng);
        const std::size_t before = chain->graph.incrementalCount();
        EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, longest)) << "dab " << dab;
        const std::size_t made = longestOf(*strands) == was ? 5u : 4u;  // (the longest changed: all but Noise)
        if (dab > 0) EXPECT_EQ(chain->graph.incrementalCount() - before, made) << "dab " << dab;
    }
    Graph::setDiffFullResults(false);
    strands = longestLengthened(*strands);
    std::size_t before = chain->graph.incrementalCount();
    EXPECT_EQ(chain->run(strands)->contentHash(), freshHash(strands, longest));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 1u);  // TestDiff alone
    Graph::setDiffFullResults(true);
}

// Hands on the guides the host set as the external "paths".
class HeldGuidesOp : public Operator {
public:
    std::string typeName() const override { return "TestHeldGuides"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    PortSpec output() const override { return {"guides", PortType::Guides}; }
    std::vector<std::string> externalInputs() const override { return {"paths"}; }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override { return ctx.external("paths"); }
};

// held -> diff, then two looks of it (noise, curl) blended by the mask, merged with a third
// (length), culled, a switch (picking the cull), a Tangle (no incremental path: it reads
// neighbors) and a Braid along held paths -> out.
std::unique_ptr<Chain> makeUtilityChain(const ChainEdit& edit = {}) {
    auto chain = std::make_unique<Chain>();
    Graph& g = chain->graph;
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        chain->ids[name] = g.addNode(std::move(op));
        g.setLabel(chain->ids[name], name);
    };
    auto& registry = OperatorRegistry::instance();
    add("held", std::make_unique<HeldStrandsOp>());
    add("diff", std::make_unique<DiffOp>());
    for (const char* type : {op::kNoise, op::kCurl, op::kLength, op::kBlend, op::kMerge, op::kCull, op::kSwitch, op::kTangle,
                             op::kBraid, op::kOutput}) {
        add(type, registry.create(type));
    }
    add("paths", std::make_unique<HeldGuidesOp>());
    chain->node(op::kNoise).params().set("amplitude", 0.05);
    chain->node(op::kCurl).params().set("radius", 0.02);
    chain->node(op::kLength).params().set("keep", 0.7);
    MapRef mask;
    mask.source = MapRef::Source::Attribute;
    mask.name = "mask";
    chain->node(op::kBlend).params().set("mix", mask);
    chain->node(op::kCull).params().set("keep", 0.6);
    chain->node(op::kTangle).params().set("radius", 0.3);
    chain->node(op::kBraid).params().set("region", 0.6);
    auto link = [&](const char* from, const char* to, std::size_t slot) { g.connect(chain->ids[from], chain->ids[to], slot); };
    link("held", "diff", 0);
    link("diff", op::kNoise, 0);
    link("diff", op::kCurl, 0);
    link("diff", op::kLength, 0);
    link(op::kNoise, op::kBlend, 0);
    link(op::kCurl, op::kBlend, 1);
    link(op::kBlend, op::kMerge, 0);
    link(op::kLength, op::kMerge, 1);
    link(op::kMerge, op::kCull, 0);
    link(op::kCull, op::kSwitch, 0);
    link("diff", op::kSwitch, 1);
    link(op::kSwitch, op::kTangle, 0);
    link(op::kTangle, op::kBraid, 0);
    link("paths", op::kBraid, 1);
    link(op::kBraid, op::kOutput, 0);
    chain->ids["out"] = chain->ids[op::kOutput];
    GuideSet paths;
    paths.add(1, test::straightCurve(Vec3f(0.2f, 0.0f, 0.1f), Vec3f(0.2f, -1.5f, 0.4f), 8));
    paths.add(2, test::straightCurve(Vec3f(-0.5f, 0.3f, -0.2f), Vec3f(-0.7f, -1.2f, 0.0f), 8));
    const auto held = std::make_shared<const GuideSet>(std::move(paths));
    chain->ext.set("paths", GuidesPtr(held), held->contentHash());
    if (edit) edit(*chain);
    return chain;
}

TEST(Incremental, SwitchMergeBlendCullAndBraidFollowDabs) {
    // Blend mixes again the strands either look changed in, Merge writes each side's changed
    // strands over their places, Cull cuts again its kept strands that changed, Switch hands on
    // its picked input's changes, Braid braids again the changed strands along the same paths;
    // Tangle (its neighbors) evaluates in full and says which strands its result changed in.
    const Verified verified;
    auto chain = makeUtilityChain();
    auto fresh = [](const StrandsPtr& strands, const ChainEdit& edit = {}) { return makeUtilityChain(edit)->run(strands)->contentHash(); };
    StrandsPtr strands = someStrands(400, 21);
    ASSERT_EQ(chain->run(strands)->contentHash(), fresh(strands));
    Rng rng(8);
    for (int dab = 0; dab < 6; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 2 + static_cast<std::size_t>(dab) * 6, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands)) << "dab " << dab;
        // TestDiff, Noise, Curl, Length, Blend, Merge, Cull, Switch, Braid.
        EXPECT_EQ(chain->graph.incrementalCount() - before, 9u) << "dab " << dab;
    }
    // A keep read from the mask a dab changes: a kept strand may go (Cull in full, then the nodes
    // after it), results as ever.
    const ChainEdit masked = [](Chain& c) {
        MapRef mask;
        mask.source = MapRef::Source::Attribute;
        mask.name = "mask";
        c.node(op::kCull).params().set("keep", mask);
    };
    masked(*chain);
    for (int dab = 0; dab < 6; ++dab) {
        strands = dabbed(*strands, 8, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands, masked)) << "dab " << dab;
    }
    // The other input switched to, merged with a seam: still a fresh chain's results.
    const ChainEdit seam = [&](Chain& c) {
        masked(c);
        c.node(op::kSwitch).params().set("index", std::int64_t{1});
        c.node(op::kMerge).params().set("blend", 0.2);
    };
    seam(*chain);
    for (int dab = 0; dab < 3; ++dab) {
        strands = dabbed(*strands, 5, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands, seam)) << "dab " << dab;
    }
}

// held -> diff -> split (a, b, c by thirds), then a -> noise, b -> curl -> length, merged, and c
// merged in as it comes -> out.
std::unique_ptr<Chain> makeSplitChain(const ChainEdit& edit = {}) {
    auto chain = std::make_unique<Chain>();
    Graph& g = chain->graph;
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        chain->ids[name] = g.addNode(std::move(op));
        g.setLabel(chain->ids[name], name);
    };
    auto& registry = OperatorRegistry::instance();
    add("held", std::make_unique<HeldStrandsOp>());
    add("diff", std::make_unique<DiffOp>());
    add("split", registry.create(op::kSplit));
    add("noise", registry.create(op::kNoise));
    add("curl", registry.create(op::kCurl));
    add("length", registry.create(op::kLength));
    add("merge", registry.create(op::kMerge));
    add("merge2", registry.create(op::kMerge));
    add("out", registry.create(op::kOutput));
    chain->node("split").params().set("weightC", 1.0);
    chain->node("noise").params().set("amplitude", 0.05);
    chain->node("curl").params().set("radius", 0.02);
    chain->node("length").params().set("keep", 0.7);
    auto link = [&](const char* from, const char* to, std::size_t slot, std::size_t output = 0) {
        g.connect(chain->ids[from], chain->ids[to], slot, output);
    };
    link("held", "diff", 0);
    link("diff", "split", 0);
    link("split", "noise", 0, 0);
    link("split", "curl", 0, 1);
    link("curl", "length", 0);
    link("noise", "merge", 0);
    link("length", "merge", 1);
    link("merge", "merge2", 0);
    link("split", "merge2", 1, 2);
    link("merge2", "out", 0);
    if (edit) edit(*chain);
    return chain;
}

std::size_t splitOutputSize(const Chain& c, std::size_t output) {
    const StrandsPtr strands = valueAs<StrandsPtr>(c.graph.cachedValue(c["split"], output));
    return strands ? strands->strandCount() : 0;
}

TEST(Incremental, ExpressionsOfIndexFollowDabsExactly) {
    // `index` is a strand's place in its set (0 to 1): a node evaluates incrementally only on the
    // strands its last result was made from (the same IDs in the same places), where every
    // strand's index is what it was, so an expression of it reads only the strand itself. Where a
    // dab changes which strands a set holds (a Cull's kept strands, a Split's outputs), the nodes
    // after it evaluate in full. Every incremental result checked against a full evaluation.
    const Verified verified;
    auto expression = [](const char* text) {
        MapRef map;
        map.source = MapRef::Source::Expression;
        map.name = text;
        return map;
    };
    const ChainEdit indexed = [&](Chain& c) {
        c.node(op::kNoise).params().set("amplitude", expression("0.02 + 0.06 * index"));
        c.node(op::kLength).params().set("keep", expression("0.5 + 0.5 * index"));
        c.node(op::kCull).params().set("keep", expression("mask > 0.3 ? 1 : 0.5 * index"));
    };
    auto chain = makeUtilityChain(indexed);
    auto fresh = [&](const StrandsPtr& strands) { return makeUtilityChain(indexed)->run(strands)->contentHash(); };
    StrandsPtr strands = someStrands(400, 31);
    ASSERT_EQ(chain->run(strands)->contentHash(), fresh(strands));
    Rng rng(12);
    const std::size_t before = chain->graph.incrementalCount();
    for (int dab = 0; dab < 8; ++dab) {
        strands = dabbed(*strands, 3 + static_cast<std::size_t>(dab) * 5, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands)) << "dab " << dab;
    }
    EXPECT_GE(chain->graph.incrementalCount() - before, 8u * 4u);  // TestDiff, Noise, Length, Blend at least
    // Split by a map of the mask a dab changes: strands move between its outputs, and the index
    // the Noise after output a reads changes for every strand after them there.
    const ChainEdit split = [&](Chain& c) {
        c.node("split").params().set("mode", std::string("map"));
        c.node("split").params().set("select", expression("0.5 * mask + 0.5 * index"));
        c.node("noise").params().set("amplitude", expression("0.02 + 0.06 * index"));
        c.node("length").params().set("keep", expression("index"));
    };
    auto splitChain = makeSplitChain(split);
    strands = someStrands(300, 32);
    ASSERT_EQ(splitChain->run(strands)->contentHash(), makeSplitChain(split)->run(strands)->contentHash());
    for (int dab = 0; dab < 8; ++dab) {
        strands = dabbed(*strands, 4 + static_cast<std::size_t>(dab) * 3, rng);
        EXPECT_EQ(splitChain->run(strands)->contentHash(), makeSplitChain(split)->run(strands)->contentHash()) << "dab " << dab;
    }
}

TEST(Incremental, SplitHandsEachOutputItsOwnChangedStrands) {
    // A dab upstream of Split reaches its outputs a, b and c: each writes the changed strands it
    // holds over their places, says them in its own indices, and the looks on each branch and
    // the merges follow them.
    const Verified verified;
    auto chain = makeSplitChain();
    auto fresh = [](const StrandsPtr& strands, const ChainEdit& edit = {}) { return makeSplitChain(edit)->run(strands)->contentHash(); };
    StrandsPtr strands = someStrands(600, 31);
    ASSERT_EQ(chain->run(strands)->contentHash(), fresh(strands));
    for (std::size_t k = 0; k < 3; ++k) EXPECT_GT(splitOutputSize(*chain, k), 100u) << "output " << k;
    Rng rng(4);
    for (int dab = 0; dab < 5; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 3 + static_cast<std::size_t>(dab) * 9, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands)) << "dab " << dab;
        // TestDiff, Split, Noise, Curl, Length and both Merges.
        EXPECT_EQ(chain->graph.incrementalCount() - before, 7u) << "dab " << dab;
    }
    // A param edit on one branch after Split: that branch in full, with the dab; Split and the
    // other branch go on incrementally, and so do the merges (the branch made anew holds the
    // strands it held: they write all of its strands); the next dab is incremental throughout.
    const ChainEdit curlier = [](Chain& c) { c.node("curl").params().set("radius", 0.04); };
    curlier(*chain);
    std::size_t before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 6, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands, curlier));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 5u);  // TestDiff, Split, Noise and both Merges
    before = chain->graph.incrementalCount();
    strands = dabbed(*strands, 6, rng);
    EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands, curlier));
    EXPECT_EQ(chain->graph.incrementalCount() - before, 7u);
    // Every strand to a: a is the input itself and changes as it does; b and c stay empty (the
    // Merges hand a on as it comes, its changes with it).
    const ChainEdit allToA = [&](Chain& c) {
        curlier(c);
        c.node("split").params().set("weightB", 0.0);
        c.node("split").params().set("weightC", 0.0);
    };
    allToA(*chain);
    for (int dab = 0; dab < 3; ++dab) {
        before = chain->graph.incrementalCount();
        const std::uint64_t serial = chain->graph.resultSerial((*chain)["out"]);
        strands = dabbed(*strands, 5, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands, allToA)) << "dab " << dab;
        EXPECT_EQ(splitOutputSize(*chain, 0), strands->strandCount());
        if (dab == 0) continue;
        // TestDiff, Split, Noise, and Curl and Length on b (empty, unchanged).
        EXPECT_EQ(chain->graph.incrementalCount() - before, 5u) << "dab " << dab;
        const ChangesPtr changes = chain->graph.changesSince((*chain)["out"], serial);
        ASSERT_TRUE(changes);
        EXPECT_FALSE(changes->all);
        EXPECT_FALSE(changes->strands.empty());
    }
}

TEST(Incremental, StrandsMovingBetweenSplitOutputsLeaveAndJoinThem) {
    // Split by a map of the mask a dab changes: a strand whose mask crosses a share's bound moves
    // to another output. The two outputs it left and joined are cut again in full (their strands
    // changed), the third goes on incrementally, and every result is a fresh chain's.
    const Verified verified;
    const ChainEdit byMask = [](Chain& c) {
        MapRef mask;
        mask.source = MapRef::Source::Attribute;
        mask.name = "mask";
        c.node("split").params().set("mode", std::string("map"));
        c.node("split").params().set("select", mask);
    };
    auto chain = makeSplitChain(byMask);
    auto fresh = [&](const StrandsPtr& strands) { return makeSplitChain(byMask)->run(strands)->contentHash(); };
    StrandsPtr strands = someStrands(500, 12);
    ASSERT_EQ(chain->run(strands)->contentHash(), fresh(strands));
    // Points moved alone: no strand changes output, everything incremental.
    {
        auto moved = std::make_shared<StrandSet>(*strands);
        for (const std::size_t s : {3u, 77u, 140u, 401u}) moved->strandPoints(s)[4] += Vec3f(0.05f, 0.0f, 0.0f);
        strands = moved;
        const std::size_t before = chain->graph.incrementalCount();
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands));
        EXPECT_EQ(chain->graph.incrementalCount() - before, 7u);
    }
    // One strand of a (mask under a third) moved to c, another of a moved too but kept in a.
    {
        const std::vector<float>& mask = *strands->strandChannel("mask");
        std::size_t leaving = 0, staying = 0;
        while (mask[leaving] >= 0.3f) ++leaving;
        staying = leaving + 1;
        while (mask[staying] >= 0.3f) ++staying;
        auto moved = std::make_shared<StrandSet>(*strands);
        (*moved->mutableStrandChannel("mask"))[leaving] = 0.9f;
        moved->strandPoints(staying)[2] += Vec3f(0.0f, 0.0f, 0.05f);
        moved->strandPoints(leaving)[3] += Vec3f(0.0f, 0.03f, 0.0f);
        const std::size_t a = splitOutputSize(*chain, 0), b = splitOutputSize(*chain, 1), c = splitOutputSize(*chain, 2);
        strands = moved;
        const std::size_t before = chain->graph.incrementalCount();
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands));
        EXPECT_EQ(splitOutputSize(*chain, 0), a - 1);
        EXPECT_EQ(splitOutputSize(*chain, 1), b);
        EXPECT_EQ(splitOutputSize(*chain, 2), c + 1);
        // TestDiff, Split, and b's Curl and Length; a's Noise and both Merges in full.
        EXPECT_EQ(chain->graph.incrementalCount() - before, 4u);
    }
    // Random dabs, the mask changing in some: strands come and go, results as ever.
    Rng rng(6);
    for (int dab = 0; dab < 6; ++dab) {
        strands = dabbed(*strands, 4 + static_cast<std::size_t>(dab) * 5, rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), fresh(strands)) << "dab " << dab;
    }
}

TEST(Incremental, VerificationCatchesAnOperatorThatGetsItWrong) {
    const Verified verified;
    auto chain = makeChain({}, true);
    StrandsPtr strands = someStrands(50, 6);
    chain->run(strands);
    Rng rng(1);
    strands = dabbed(*strands, 3, rng);
    try {
        chain->run(strands);
        FAIL() << "a stale incremental result went unnoticed";
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("evaluated incrementally"), std::string::npos) << e.what();
    }
}

// ---- Brush strokes on a session ------------------------------------------------------------------

// `count` primaries of 7 points along the unit sphere's normals, each with a painted "clump" value
// and a "tint" layer, and `children` children each, rooted.
std::string sphereGuides(const ScalpMesh& scalp, int count, std::size_t children) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(3);
    std::vector<GuideId> ids;
    for (int i = 0; i < count; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(0.2f, 1), rng.uniform(-1, 1)).normalized();
        ids.push_back(h.addGuide(primary, test::straightCurve(n, n * 0.7f + Vec3f(0.15f, -0.1f, 0.0f), 6)));
    }
    h.evaluate(&scalp);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        h.setAttribute(ids[i], "clump", rng.uniform(0, 1));
        h.setPointAttribute(ids[i], "tint", {rng.uniform(0, 1), rng.uniform(0, 1), rng.uniform(0, 1)});
    }
    if (children > 0) {
        ChildGenerationParams params;
        params.count = children;
        h.generateChildren(primary, "children", params);
    }
    h.evaluate(&scalp);
    return h.toJson();
}

GroomData sphereGroom(const std::string& graph, int guides, std::size_t children) {
    const auto scalp = test::makeSphere(24, 48, 1.0f);
    GroomData data;
    data.scalp = scalp;
    data.graphJson = graph;
    data.hierarchyJson = sphereGuides(*scalp, guides, children);
    return data;
}

// A look graph: Interpolate and Clump, its own centers (its strength from the guides' painted
// clump value), Noise, Curl, Width along a guide layer (per point), Length, Resample, Output.
const char* kLookGraph = R"({"version": 1, "output": "out",
  "nodes": [
    {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 700.0}},
    {"name": "interp", "type": "Interpolate"},
    {"name": "clump", "type": "Clump", "params": {"count": 30, "strength": {"attribute": "clump", "min": 0.2, "max": 0.9}}},
    {"name": "noise", "type": "Noise", "params": {"amplitude": 0.02, "frequency": 4.0}},
    {"name": "curl", "type": "Curl", "params": {"radius": 0.01}},
    {"name": "width", "type": "Width", "params": {"width": {"attribute": "tint", "max": 0.01}}},
    {"name": "length", "type": "Transform", "params": {"scale": 0.9, "scaleRandom": 0.2}},
    {"name": "resample", "type": "Resample", "params": {"segments": 9, "mode": "even"}},
    {"name": "out", "type": "Output"}],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interp", "input": "strands"},
    {"from": "guides", "to": "interp", "input": "guides"},
    {"from": "scalp", "to": "interp", "input": "scalp"},
    {"from": "interp", "to": "clump", "input": "strands"},
    {"from": "clump", "to": "noise", "input": "strands"},
    {"from": "noise", "to": "curl", "input": "strands"},
    {"from": "curl", "to": "width", "input": "strands"},
    {"from": "width", "to": "length", "input": "strands"},
    {"from": "length", "to": "resample", "input": "strands"},
    {"from": "resample", "to": "out", "input": "strands"}]})";

// Bend combing, HairColor with a tip blend (per-point colors), Resample by curvature (each strand's count
// by its turning) and then evenly to fewer points (a new layout, kept from dab to dab).
const char* kShapeGraph = R"({"version": 1, "output": "out",
  "nodes": [
    {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 700.0}},
    {"name": "interp", "type": "Interpolate"},
    {"name": "comb", "type": "Bend", "params": {"angle": 25.0, "along": "direction", "direction": [1.0, -1.0, 0.0]}},
    {"name": "curvature", "type": "Resample", "params": {"segments": 12, "mode": "curvature", "maxAngle": 4.0}},
    {"name": "color", "type": "HairColor", "params": {"tipStrength": 0.4, "melanin": {"attribute": "clump"}, "melaninRandom": 0.2}},
    {"name": "even", "type": "Resample", "params": {"segments": 6, "mode": "even"}},
    {"name": "out", "type": "Output"}],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interp", "input": "strands"},
    {"from": "guides", "to": "interp", "input": "guides"},
    {"from": "scalp", "to": "interp", "input": "scalp"},
    {"from": "interp", "to": "comb", "input": "strands"},
    {"from": "comb", "to": "curvature", "input": "strands"},
    {"from": "curvature", "to": "color", "input": "strands"},
    {"from": "color", "to": "even", "input": "strands"},
    {"from": "even", "to": "out", "input": "strands"}]})";

// Interpolate, then Collide (the scalp and a collider mesh above it), Lift, Part, Flow (along an
// arrow), Smooth, Flyaway, Attribute (a map of the guides' clump value), Wind, Jiggle (the scalp
// moved over the last frames), RestoreLength (to Interpolate's lengths), Instance, and last Twist
// around the guides (which a stroke changes: in full).
const char* kDeformGraph = R"({"version": 1, "output": "out",
  "nodes": [
    {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"}, {"name": "collider", "type": "ColliderMesh"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 700.0}},
    {"name": "interp", "type": "Interpolate"},
    {"name": "collide", "type": "Collide", "params": {"offset": 0.04, "maxDepth": 0.4, "iterations": 3}},
    {"name": "lift", "type": "Bend", "params": {"angle": 15.0, "end": 0.0}},
    {"name": "part", "type": "Part", "params": {"width": 0.4}},
    {"name": "flow", "type": "Flow", "params": {"affect": 0.4}},
    {"name": "smooth", "type": "Smooth"},
    {"name": "flyaway", "type": "Flyaway", "params": {"fraction": 0.2}},
    {"name": "mask", "type": "Attribute", "params": {"name": "painted", "value": {"attribute": "clump"}}},
    {"name": "twist", "type": "Twist", "params": {"turns": 0.4}},
    {"name": "wind", "type": "Wind", "params": {"strength": 0.2}},
    {"name": "jiggle", "type": "Jiggle"},
    {"name": "restore", "type": "RestoreLength", "params": {"affect": 0.6}},
    {"name": "instance", "type": "Instance", "params": {"probability": 0.2}},
    {"name": "out", "type": "Output"}],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interp", "input": "strands"},
    {"from": "guides", "to": "interp", "input": "guides"},
    {"from": "scalp", "to": "interp", "input": "scalp"},
    {"from": "interp", "to": "collide", "input": "strands"},
    {"from": "scalp", "to": "collide", "input": "scalp"},
    {"from": "collider", "to": "collide", "input": "collider"},
    {"from": "collide", "to": "lift", "input": "strands"},
    {"from": "lift", "to": "part", "input": "strands"},
    {"from": "part", "to": "flow", "input": "strands"},
    {"from": "flow", "to": "smooth", "input": "strands"},
    {"from": "smooth", "to": "flyaway", "input": "strands"},
    {"from": "flyaway", "to": "mask", "input": "strands"},
    {"from": "mask", "to": "wind", "input": "strands"},
    {"from": "wind", "to": "jiggle", "input": "strands"},
    {"from": "jiggle", "to": "restore", "input": "strands"},
    {"from": "interp", "to": "restore", "input": "reference"},
    {"from": "restore", "to": "instance", "input": "strands"},
    {"from": "instance", "to": "twist", "input": "strands"},
    {"from": "guides", "to": "twist", "input": "axes"},
    {"from": "twist", "to": "out", "input": "strands"}]})";

// held -> diff -> transform -> direction -> bend -> cut (a collider) -> render -> clump (guides as
// centers, varied per clump) -> out, on strands scattered on a sphere (rooted, so each node's
// root frame is the scalp's).
std::unique_ptr<Chain> makeParityChain() {
    auto chain = std::make_unique<Chain>();
    Graph& g = chain->graph;
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        chain->ids[name] = g.addNode(std::move(op));
        g.setLabel(chain->ids[name], name);
    };
    auto& registry = OperatorRegistry::instance();
    add("held", std::make_unique<HeldStrandsOp>());
    add("diff", std::make_unique<DiffOp>());
    for (const char* type : {op::kTransform, op::kDirection, op::kBend, op::kMeshCut, op::kRenderData, op::kHairColor, op::kClump, op::kGuides,
                             op::kColliderMesh, op::kOutput}) {
        add(type, registry.create(type));
    }
    chain->node(op::kTransform).params().set("rotateRandom", Vec3f{5.0f, 20.0f, 0.0f});
    chain->node(op::kTransform).params().set("scaleRandom", 0.2);
    chain->node(op::kDirection).params().set("strength", 0.5);
    chain->node(op::kDirection).params().set("rollRandom", 40.0);
    chain->node(op::kBend).params().set("angle", -50.0);
    chain->node(op::kBend).params().set("randomness", 0.5);
    chain->node(op::kColliderMesh).params().set("name", std::string("collider"));
    chain->node(op::kRenderData).params().set("opacityProfile", Ramp::linear(1.0f, 0.3f));
    MapRef byLength;
    byLength.source = MapRef::Source::Expression;
    byLength.name = "length";  // reads each strand's points: the strands a dab moved are written again
    chain->node(op::kRenderData).params().set("opacity", byLength);
    chain->node(op::kHairColor).params().set("roughness", byLength);
    chain->node(op::kHairColor).params().set("specularRandom", 0.3);
    chain->node(op::kHairColor).params().set("tilt", 4.0);
    chain->node(op::kClump).params().set("volumize", 0.5);
    chain->node(op::kClump).params().set("strengthRandom", 0.5);
    chain->node(op::kClump).params().set("copyAmount", 0.5);
    chain->node(op::kClump).params().set("centersFrom", std::string("inputPoints"));  // the guides below
    const char* order[] = {"held", "diff", op::kTransform, op::kDirection, op::kBend, op::kMeshCut, op::kRenderData, op::kHairColor, op::kClump,
                           op::kOutput};
    for (std::size_t k = 0; k + 1 < std::size(order); ++k) g.connect(chain->ids[order[k]], chain->ids[order[k + 1]], 0);
    g.connect((*chain)[op::kColliderMesh], (*chain)[op::kMeshCut], 1);
    g.connect((*chain)[op::kGuides], (*chain)[op::kClump], 1);
    chain->ids["out"] = chain->ids[op::kOutput];
    const auto scalp = test::makeSphere(16, 32, 0.9f);
    chain->ext.set("scalp", MeshPtr(scalp), scalp->contentHash());
    std::vector<Vec3f> points = test::makeSphere(10, 20, 0.5f)->points();
    for (Vec3f& p : points) p += Vec3f(0.3f, 1.2f, 0.1f);
    const auto collider = std::make_shared<const ScalpMesh>(std::move(points), test::makeSphere(10, 20, 0.5f)->triangles());
    chain->ext.set("mesh:collider", MeshPtr(collider), collider->contentHash());
    auto centers = std::make_shared<GuideSet>();
    Rng rng(5);
    for (std::uint64_t i = 0; i < 12; ++i) {
        const Vec3f root = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized() * 0.9f;
        centers->add(i + 1, test::straightCurve(root, root * 0.5f, 6));
    }
    chain->ext.set("guides", GuidesPtr(centers), 0xC3E7E25ull);
    return chain;
}

// Strands scattered on the parity chain's sphere, with the "mask" channel the dabs repaint.
StrandsPtr scatteredOnSphere() {
    Graph g;
    ExternalInputs ext;
    const auto scalp = test::makeSphere(16, 32, 0.9f);
    ext.set("scalp", MeshPtr(scalp), scalp->contentHash());
    const NodeId scalpNode = g.addNode(op::kScalp), scatter = g.addNode(op::kScatter);
    g.node(scatter).params().set("density", 60.0);
    g.connect(scalpNode, scatter, 0);
    EvalContext ctx;
    ctx.externals = &ext;
    auto strands = std::make_shared<StrandSet>(*g.evaluateStrands(scatter, ctx));
    strands->addStrandChannel("mask", 0.5f);
    return strands;
}

TEST(Incremental, TheParityNodesFollowRandomDabs) {
    // Transform, Direction, Bend, MeshCut (its collider the same), RenderData and HairColor (a map reading each
    // strand's points) and a Clump varied per clump (volumize, variance, copy, clump noise) make
    // again only the strands a dab moved: every result a fresh chain's.
    const Verified verified;
    auto chain = makeParityChain();
    StrandsPtr strands = scatteredOnSphere();
    ASSERT_GT(strands->strandCount(), 300u);
    ASSERT_EQ(chain->run(strands)->contentHash(), makeParityChain()->run(strands)->contentHash());
    Rng rng(21);
    for (int dab = 0; dab < 8; ++dab) {
        const std::size_t before = chain->graph.incrementalCount();
        strands = dabbed(*strands, 1 + static_cast<std::size_t>(rng.nextFloat() * 30.0f), rng);
        EXPECT_EQ(chain->run(strands)->contentHash(), makeParityChain()->run(strands)->contentHash()) << "dab " << dab;
        // TestDiff and the 7 operators after it (Output hands its input on).
        EXPECT_EQ(chain->graph.incrementalCount() - before, 8u) << "dab " << dab;
    }
}

// Fur grown from one guide group, whiskers made of another (ToStrands), merged, then a Width:
// the wolf's layout (Guides by group, Merge, a node after it).
const char* kTwoGroupGraph = R"({"version": 1, "output": "out",
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "furGuides", "type": "Guides", "params": {"group": "fur"}},
    {"name": "scatter", "type": "Scatter", "params": {"density": 400.0}},
    {"name": "interp", "type": "Interpolate"},
    {"name": "whiskerGuides", "type": "Guides", "params": {"group": "whiskers"}},
    {"name": "toStrands", "type": "ToStrands"},
    {"name": "whiskerWidth", "type": "Width", "params": {"width": 0.002}},
    {"name": "merge", "type": "Merge"},
    {"name": "width", "type": "Width", "params": {"width": 0.004}},
    {"name": "out", "type": "Output"}],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interp", "input": "strands"},
    {"from": "furGuides", "to": "interp", "input": "guides"},
    {"from": "scalp", "to": "interp", "input": "scalp"},
    {"from": "whiskerGuides", "to": "toStrands", "input": "guides"},
    {"from": "toStrands", "to": "whiskerWidth", "input": "strands"},
    {"from": "interp", "to": "merge", "input": "a"},
    {"from": "whiskerWidth", "to": "merge", "input": "b"},
    {"from": "merge", "to": "width", "input": "strands"},
    {"from": "width", "to": "out", "input": "strands"}]})";

GuideHierarchy twoGroups(const ScalpMesh& scalp) {
    GuideHierarchy h;
    const GroupId fur = h.addGroup("fur"), whiskers = h.addGroup("whiskers");
    Rng rng(9);
    for (int i = 0; i < 24; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(0.2f, 1), rng.uniform(-1, 1)).normalized();
        h.addGuide(fur, test::straightCurve(n, n * 0.6f + Vec3f(0.1f, -0.1f, 0.0f), 6));
    }
    for (int i = 0; i < 4; ++i) {
        const Vec3f n = Vec3f(0.3f * static_cast<float>(i) - 0.45f, 0.4f, 1.0f).normalized();
        h.addGuide(whiskers, test::straightCurve(n, n * 0.9f, 8));
    }
    h.evaluate(&scalp);
    // Bound again from where they sit, as a brushed guide is (setPoints): the roots an edit binds
    // them to from then on, to the bit (bound from the curves as drawn, the first time, they may
    // differ in the last bits).
    for (const HierarchyGuide& g : std::vector<HierarchyGuide>(h.guides())) h.setPoints(g.id, g.points);
    h.evaluate(&scalp);
    return h;
}

// The hierarchy with every guide of `group` moved a little (more toward the tip).
std::shared_ptr<GuideHierarchy> movedGroup(const GuideHierarchy& from, const std::string& group, float by) {
    auto h = std::make_shared<GuideHierarchy>(from);
    const GroupId id = h->findGroup(group);
    for (const HierarchyGuide& g : from.guides()) {
        if (g.group != id) continue;
        std::vector<Vec3f> points = g.points;
        for (std::size_t k = 1; k < points.size(); ++k) points[k] += Vec3f(by * static_cast<float>(k), 0.0f, 0.0f);
        h->setPoints(g.id, std::move(points));
    }
    return h;
}

TEST(Incremental, AGuidesNodeIsKeyedByItsOwnGroups) {
    // A dab on the whiskers' guides (a brush's preview guides) leaves the fur's Guides node and its
    // branch cached (their key is the fur group's guides, not the whole hierarchy's), and the other
    // way round; Merge and the Width after it go on incrementally, writing the side that changed.
    const Verified verified;
    const auto scalp = test::makeSphere(24, 48, 1.0f);
    const GuideHierarchy base = twoGroups(*scalp);
    GroomData data;
    data.scalp = scalp;
    data.graphJson = kTwoGroupGraph;
    GroomEvaluator session(data);
    session.setHierarchy(std::make_shared<const GuideHierarchy>(base));
    // A fresh session's view of the same guides (every node in full).
    auto fresh = [&](const std::shared_ptr<GuideHierarchy>& h) {
        GroomEvaluator other(data);
        other.setHierarchy(std::make_shared<const GuideHierarchy>(base));
        other.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*h));
        const GroomEvaluator::Result& view = other.evaluate(GroomEvaluator::Request{});
        EXPECT_TRUE(view.errors.empty());
        return view.strands ? view.strands->contentHash() : 0;
    };
    GroomEvaluator::Request request;
    const GroomEvaluator::Result& first = session.evaluate(request);
    ASSERT_TRUE(first.errors.empty()) << first.errors[0];
    const std::size_t whiskerStrands = 4, all = first.strands->strandCount();
    ASSERT_GT(all, 10 * whiskerStrands);

    auto runOf = [&](const std::string& node) {
        const auto runs = session.stats().runs;
        const auto it = runs.find(node);
        return it == runs.end() ? std::string() : it->second;
    };
    // The whiskers moved: their branch made again; the fur's is not looked at past Merge's input.
    // The guides as the session shows them (a stroke's preview starts from them), previewed as
    // they are first (the press).
    const GuideHierarchy shown = *session.hierarchy();
    session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(shown));
    ASSERT_TRUE(session.evaluate(request).errors.empty());
    std::size_t evaluations = session.evaluationCount();
    const auto whiskersMoved = movedGroup(shown, "whiskers", 0.01f);
    session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*whiskersMoved));
    const GroomEvaluator::Result& whiskers = session.evaluate(request);
    ASSERT_TRUE(whiskers.errors.empty()) << whiskers.errors[0];
    EXPECT_EQ(whiskers.strands->contentHash(), fresh(whiskersMoved));
    EXPECT_EQ(runOf("merge"), "incremental " + std::to_string(whiskerStrands)) << "the whiskers' side, every strand of it";
    EXPECT_EQ(runOf("width"), "incremental " + std::to_string(whiskerStrands));
    EXPECT_EQ(runOf("whiskerGuides").rfind("full", 0), 0u) << runOf("whiskerGuides");
    // whiskerGuides, toStrands, whiskerWidth, merge, width (and Output).
    EXPECT_LE(session.evaluationCount() - evaluations, 6u) << "the fur's branch stays cached";

    // The fur moved: the whiskers' branch cached, Merge writes the fur strands Interpolate made again.
    evaluations = session.evaluationCount();
    const auto furMoved = movedGroup(*whiskersMoved, "fur", 0.005f);
    session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*furMoved));
    const GroomEvaluator::Result& fur = session.evaluate(request);
    ASSERT_TRUE(fur.errors.empty()) << fur.errors[0];
    EXPECT_EQ(fur.strands->contentHash(), fresh(furMoved));
    EXPECT_EQ(runOf("whiskerWidth"), "cached");
    EXPECT_EQ(runOf("furGuides").rfind("full", 0), 0u) << runOf("furGuides");
    EXPECT_EQ(runOf("merge").rfind("incremental", 0), 0u) << runOf("merge");
    EXPECT_LE(session.evaluationCount() - evaluations, 6u) << "furGuides, interp, merge, width (the whiskers' branch cached)";

    // A whisker added: Merge's sides no longer hold the strands its last result held (in full).
    auto added = std::make_shared<GuideHierarchy>(*furMoved);
    const Vec3f n = Vec3f(0.6f, 0.3f, 1.0f).normalized();
    added->addGuide(added->findGroup("whiskers"), test::straightCurve(n, n * 0.8f, 8));
    added->evaluate(scalp.get());
    session.setPreviewHierarchy(std::make_shared<GuideHierarchy>(*added));
    const GroomEvaluator::Result& more = session.evaluate(request);
    ASSERT_TRUE(more.errors.empty()) << more.errors[0];
    EXPECT_EQ(more.strands->strandCount(), all + 1);
    EXPECT_EQ(more.strands->contentHash(), fresh(added));
    EXPECT_EQ(runOf("merge").rfind("full", 0), 0u) << runOf("merge");
}

// Hands on the strands the host set as the external "other" (a second side's).
class HeldOtherOp : public HeldStrandsOp {
public:
    std::string typeName() const override { return "TestHeldOther"; }
    std::vector<std::string> externalInputs() const override { return {"other"}; }
    Value evaluate(const EvalContext& ctx, const std::vector<Value>&) const override { return ctx.external("other"); }
};

TEST(Incremental, MergeWritesASideMadeAnewAsThatSidesStrands) {
    // One side of a Merge made in full (a node of its branch edited: no list of its changes) while
    // the other is unchanged: Merge lists that side's strands, as they are the strands it held, so
    // the node after it goes on incrementally. A side holding most strands merges in full (no
    // faster), so does one holding other strands (a count).
    const Verified verified;
    Graph g;
    std::map<std::string, NodeId> ids;
    auto& registry = OperatorRegistry::instance();
    auto add = [&](const std::string& name, std::unique_ptr<Operator> op) {
        ids[name] = g.addNode(std::move(op));
        g.setLabel(ids[name], name);
    };
    add("held", std::make_unique<HeldStrandsOp>());
    add("other", std::make_unique<HeldOtherOp>());
    add("noiseA", registry.create(op::kNoise));
    add("noiseB", registry.create(op::kNoise));
    add("merge", registry.create(op::kMerge));
    add("width", registry.create(op::kWidth));
    g.connect(ids["held"], ids["noiseA"], 0);
    g.connect(ids["noiseA"], ids["merge"], 0);
    g.connect(ids["other"], ids["noiseB"], 0);
    g.connect(ids["noiseB"], ids["merge"], 1);
    g.connect(ids["merge"], ids["width"], 0);
    g.node(ids["noiseA"]).params().set("amplitude", 0.01);
    g.node(ids["noiseB"]).params().set("amplitude", 0.01);
    g.node(ids["width"]).params().set("width", 0.003);

    // a: 12 strands (IDs from 1000), b: 200 of their own IDs (none renamed by the merge).
    const auto a = someStrands(12, 5);
    auto b = std::make_shared<StrandSet>(*someStrands(200, 6));
    for (std::size_t s = 0; s < b->strandCount(); ++s) b->mutableIds()[s] = 5000 + s;
    ExternalInputs ext;
    EvalContext ctx;
    ctx.externals = &ext;
    auto run = [&](const StrandsPtr& sideA, const StrandsPtr& sideB) {
        ext.set("strands", sideA, sideA->contentHash());
        ext.set("other", sideB, sideB->contentHash());
        // (Each incremental result checked against the operator's full one: Verified.)
        return g.evaluateStrands(ids["width"], ctx);
    };
    auto kind = [&](const char* node) { return g.lastRunKinds().at(ids[node]); };
    ASSERT_TRUE(run(a, b));

    // a's Noise edited: a made anew (12 of 212 strands), b unchanged.
    g.node(ids["noiseA"]).params().set("amplitude", 0.02);
    run(a, b);
    EXPECT_EQ(kind("noiseA").rfind("full", 0), 0u) << kind("noiseA");
    EXPECT_EQ(kind("merge"), "incremental 12");
    EXPECT_EQ(kind("width"), "incremental 12");

    // b's Noise edited: b made anew, but it holds most strands: in full.
    g.node(ids["noiseB"]).params().set("amplitude", 0.02);
    run(a, b);
    EXPECT_EQ(kind("merge").rfind("full", 0), 0u) << kind("merge");

    // a with a strand fewer: other strands, in full (and right).
    g.node(ids["noiseA"]).params().set("amplitude", 0.03);
    run(someStrands(11, 5), b);
    EXPECT_EQ(kind("merge").rfind("full", 0), 0u) << kind("merge");
}

}  // namespace
}  // namespace groomlab
