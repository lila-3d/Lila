// SPDX-License-Identifier: Apache-2.0
#pragma once

// Script nodes (docs/CUSTOM_NODES.md, phase 2): a node whose work is a short script in the
// expression language (expression.h), extended for nodes. A script changes the strands it is
// given, point by point and strand by strand: it never adds or removes strands or points, and
// never reads another strand.
//
//   # Wave: bends each strand sideways along its length.
//   per point {
//       $side = normalize(cross(tangent, N));
//       P += $side * $amplitude * sin(t * $frequency * 6.2832 + rand * 6.2832) * t;
//   }
//
// The language: everything an expression has (operators, vectors, if/else, ~110 functions, maps,
// curves, the strand's variables: t, rand, length, u, v, N, any channel by name...), plus:
//   Blocks      per strand { ... } runs once per strand, then per point { ... } for each of its
//               points, root to tip. Either may be left out; the strand block comes first.
//   Outputs     P (the point), width, color ([r, g, b]) and channel.name, each only when the
//               node's header lists it in "writes". In a point block they are the point's; in a
//               strand block width and color set every point's (what the point block then reads)
//               and channel.name is a strand channel. P is per point only. P.y = ...; sets one
//               component. They read as they are now: the incoming value until assigned.
//   Inputs      tangent (the strand's direction at the point, as it came in), prev.P (the point
//               before, as this run left it; the root's own at the root), root, tip, pointCount,
//               pointIndex, $name for each header param (a mappable one varies per strand or per
//               point as a built-in node's does; a ramp reads as $name(x)); pointP is the point as
//               it came in. In a strand block P is the root point and t is 0.
//   Functions   strandPoint(s) the strand as it came in at arc share s (0-1); guidePoint(s) its
//               guide's (the guides input's guide rooted nearest, the strand's own point without
//               one); scalpNormal(p) and closestOnScalp(p), the scalp's at the point nearest p.
//   Locals      $name = ...; start at 0 on every strand and keep their value from one point to
//               the next (a running value root to tip); one set in the strand block reads in the
//               point block. A local is a number or a vector for good.
//   Loops       for $i in 0 .. 8 { ... } runs $i = 0 to 7: bounds are whole numbers written out,
//               or a whole-number param with a max. Nested loops are fine; no recursion.
// Every block run (a strand's, a point's) has a budget of kScriptStepBudget steps: a script past
// it fails with an error, it never hangs. Results are the same on any thread count and machine.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "groomlab/expression.h"

namespace groomlab {

class GuideSet;
class ScalarField;

// A script's error: its message and where (1-based line and column of the script's text).
class ScriptError : public ExpressionError {
public:
    ScriptError(const std::string& message, int line, int column);
    int line() const { return line_; }
    int column() const { return column_; }
    const std::string& message() const { return message_; }  // without the position

private:
    std::string message_;
    int line_ = 0, column_ = 0;
};

// The steps one block run may take (a point's, a strand's).
constexpr std::size_t kScriptStepBudget = 10000;
// The largest bound a for loop may have written out.
constexpr int kScriptMaxLoop = 100000;

// A header param as the script reads it.
struct ScriptParam {
    enum class Kind { Number, Int, Bool, Choice, Color, Ramp };
    std::string name;
    Kind kind = Kind::Number;
    std::optional<double> max;  // an int's: a loop it bounds runs to it at most
};

// The names a script reads as built-in (a param may not take one), and the functions a script
// has beyond an expression's (editors list them).
const std::vector<std::string>& scriptBuiltinNames();
const std::vector<ExpressionFunction>& scriptFunctions();

class Script {
public:
    Script() = default;
    // Compiles; throws ScriptError. `writes`: "P", "width", "color", "channel.<name>".
    Script(const std::string& text, const std::vector<ScriptParam>& params, const std::vector<std::string>& writes);

    bool valid() const { return impl_ != nullptr; }
    bool writesPoints() const;
    bool writesWidths() const;
    bool writesColors() const;
    std::vector<std::string> strandChannels() const;  // written per strand
    std::vector<std::string> pointChannels() const;   // written per point
    bool readsGuides() const;
    bool readsTime() const;
    bool readsLongest() const;  // `longest`: a value of the whole set
    std::vector<std::string> channelsRead() const;  // channels it reads by name

    // A param's value for a run, in the params' order.
    struct Value {
        Vec3f value;                         // a number as x; a color
        const ScalarField* field = nullptr;  // a mappable number's, read per strand or point
        const Ramp* ramp = nullptr;          // a ramp's
    };
    struct Inputs {
        const ScalpMesh* scalp = nullptr;
        const GuideSet* guides = nullptr;
        std::vector<Value> params;
    };
    // Runs on `out`, a copy of `in` (the same strands; for an incremental run its written arrays
    // the last result's): every strand, or the ones listed, each from its incoming points.
    // Throws ScriptError when a block runs past its budget (line 0).
    void run(StrandSet& out, const StrandSet& in, const Inputs& inputs, const std::vector<std::uint32_t>* only = nullptr) const;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

}  // namespace groomlab
