// SPDX-License-Identifier: Apache-2.0
// Example: moves each strand's points out along the scalp's normal at its root, more toward the
// tip, by a distance a map, a field or an expression can drive per strand, each strand a random
// share less.
//
// The template tools/dev/new_node.py copies for a new node (every "Example" becomes its name), and
// a real operator the tests build and run (tests/engine/core/test_example_node.cpp), so it keeps
// compiling. Replace the params and evaluate() with your node's; docs/EXTENDING.md says what each
// piece is for.

#include <memory>
#include <vector>

#include "groomlab/field.h"
#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "internal.h"

namespace groomlab {

namespace {

class ExampleOp : public Operator {
public:
    // What graph JSON stores: op::kExample comes from its line in builtin_operators.inc.
    std::string typeName() const override { return op::kExample; }
    // Its role in a chain (where a preset's look starts: "source" or "grow"; else what it does).
    // Not the menu's category: that is the NodeInfo below.
    std::string category() const override { return "shape"; }
    // Curves: strands or guides, handed back as the same kind. (PortType::Strands for strands
    // only, Guides, Mesh, Field...: graph.h.)
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }
    // The externals it reads (its cache key hashes them): the scalp, which maps sample.
    std::vector<std::string> externalInputs() const override { return {"scalp"}; }

    // Every param, its default written once here (params().get...() falls back to it), with what
    // hosts show: a description, a unit, limits, a group. A number measuring something gives its
    // unit ("length", "width", "density", "perLength") so presets fit other scalps; defaults are
    // authored at a 20-unit scalp (a head in cm). Names follow docs/DEVELOPING.md's param naming
    // (xRandom, profile, strength...). Every param must be read in some state (ParamUse.*);
    // renaming or removing one breaks saved grooms (no format migrations).
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            ParamSpec{"distance", 0.1, "How far the tip moves, scene units (mappable; negative: the other way)", "length"}
                .soft(-1, 1),
            {"profile", Ramp{{0.0f, 0.0f}, {1.0f, 1.0f}}, "Distance along the length, root to tip"},
            ParamSpec{"distanceRandom", 0.0, "Each strand up to this share less, at random", "fraction"}.range(0, 1).in("Random"),
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}).in("Random").adv(),
        };
    }

    // A pure function of params(), the context and the inputs (the graph caches it). Randomness
    // comes from each strand's stable ID, never from thread order, so one thread and many give the
    // same strands to the bit (the golden tests check). To make again only what a brush dab
    // changed, override evaluateIncremental too (deform_ops.cpp DisplaceOp).
    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, op::kExample);
        const ScalpMesh* scalp = scalpOf(ctx);  // null without one (a host that hands none)
        const ScalarField distance(params(), "distance", scalp);  // a number, a map, a field...
        distance.validate(*in);
        const Ramp profile = params().getRamp("profile");
        const auto randomness = static_cast<float>(params().getDouble("distanceRandom"));
        const auto seed = static_cast<std::uint64_t>(params().getInt("seed"));

        auto out = std::make_shared<StrandSet>(*in);  // shares the input's arrays until written
        out->mutablePositions();                      // cloned once here, before the threads write
        const RampAlong along(profile, *out);         // the profile once per point index
        parallelFor(0, out->strandCount(), [&](std::size_t s) {
            float amount = distance.at(*in, s);  // read on the input: other strands move meanwhile
            if (randomness > 0.0f) amount *= 1.0f - randomness * unitFloat(hashCombine(out->ids()[s], seed ^ 0xE8A3ull));
            const std::size_t n = out->strandPointCount(s);
            if (n < 2) return;
            Vec3f* points = out->strandPoints(s);
            // Out of the scalp at its root; a strand off it (guides of no scalp), along its first segment.
            // (A direction is unitless: no scene-unit epsilon decides it, only a zero length.)
            const StrandRoot& root = out->roots()[s];
            Vec3f way = scalp && scalp->contains(root) ? scalp->normal(root) : points[1] - points[0];
            const float length = way.length();
            if (!(length > 0.0f)) return;
            way = way / length;
            for (std::size_t i = 1; i < n; ++i) points[i] += way * (amount * along(i, n));  // the root stays
        });
        return StrandsPtr(out);
    }
};

}  // namespace

// Called by registerBuiltinOperators (its line in builtin_operators.inc). The NodeInfo is what every
// host's add-node menu and param panel show: its menu category (node_catalog.cpp's ids), a line on
// what it does and the words a search finds it by that its name does not say.
void registerExampleOperator(OperatorRegistry& registry) {
    registry.registerType(op::kExample,
                          {"shape", "Moves strands out from the scalp, more toward the tip.", "offset push lift"},
                          [] { return std::make_unique<ExampleOp>(); });
}

}  // namespace groomlab
