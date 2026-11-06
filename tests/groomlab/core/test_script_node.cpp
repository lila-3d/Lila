// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Script nodes (script.h, node_library.h): the language's additions (blocks, outputs, inputs,
// loops, the step budget), the Script node (writes, incremental runs, errors handed on), .glnode
// script files in the library and in grooms, and the shipped examples.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/export_view.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/guide_set.h"
#include "groomlab/hair_color.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/script.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

std::string shippedNodes() { return (fs::path(GROOMLAB_SHIPPED_PRESETS).parent_path() / "nodes").generic_string(); }

// `count` straight strands of `points` points up the y axis, 0.25 apart, side by side on x.
std::shared_ptr<StrandSet> straightStrands(std::size_t count = 6, std::size_t points = 5) {
    auto out = std::make_shared<StrandSet>();
    for (std::size_t i = 0; i < count; ++i) {
        std::vector<Vec3f> pts;
        for (std::size_t k = 0; k < points; ++k) pts.emplace_back(0.5f * static_cast<float>(i), 0.25f * static_cast<float>(k), 0.0f);
        out->addStrand(100 + i, pts, std::vector<float>(points, 0.01f));
    }
    return out;
}

// The script run over `in` (params by kind and value, in order).
std::shared_ptr<StrandSet> runScript(const std::string& text, const std::vector<std::string>& writes, const StrandSet& in,
                                     const std::vector<ScriptParam>& params = {}, const std::vector<Script::Value>& values = {},
                                     const GuideSet* guides = nullptr, const ScalpMesh* scalp = nullptr) {
    const Script script(text, params, writes);
    auto out = std::make_shared<StrandSet>(in);
    Script::Inputs inputs;
    inputs.params = values;
    inputs.guides = guides;
    inputs.scalp = scalp;
    script.run(*out, in, inputs);
    return out;
}

// The error compiling `text` gives (line 0 and no message when it compiles).
ScriptError compileError(const std::string& text, const std::vector<std::string>& writes = {"P"},
                         const std::vector<ScriptParam>& params = {}) {
    try {
        Script(text, params, writes);
    } catch (const ScriptError& e) {
        return e;
    }
    return ScriptError("", 0, 0);
}

ScriptParam param(const std::string& name, ScriptParam::Kind kind = ScriptParam::Kind::Number, std::optional<double> max = {}) {
    ScriptParam p;
    p.name = name;
    p.kind = kind;
    p.max = max;
    return p;
}
Script::Value number(float v) {
    Script::Value value;
    value.value = Vec3f{v, 0.0f, 0.0f};
    return value;
}

// A script node file around `script` (one length param "amount" by default).
json scriptFile(const std::string& script, const std::vector<std::string>& writes = {"P"}, const std::string& id = "test.push",
                int version = 1, json params = json()) {
    if (params.is_null()) {
        params = json::array({{{"name", "amount"}, {"type", "length"}, {"label", "Amount"}, {"default", 0.5}, {"min", 0},
                               {"softMax", 2}, {"mappable", true}, {"description", "How far"}}});
    }
    return {{"glnode", 1}, {"id", id}, {"version", version}, {"label", "Push"}, {"category", "shape"},
            {"description", "Pushes strands."}, {"size", 20}, {"inputs", json::array({{{"name", "strands"}, {"type", "Strands"}}})},
            {"output", {{"name", "out"}, {"type", "Strands"}}}, {"params", params}, {"writes", writes}, {"script", script}};
}

const char* kBase = R"j({"version": 1, "output": "out",
  "nodes": [{"name": "scalp", "type": "Scalp"},
            {"name": "scatter", "type": "Scatter", "params": {"density": 150.0}},
            {"name": "out", "type": "Output"}],
  "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}]})j";

std::string plainGraph() {
    json g = json::parse(kBase);
    g["connections"].push_back({{"from", "scatter"}, {"to", "out"}, {"input", "strands"}});
    return g.dump();
}

// The graph with a node of `type` (with these params) between the scatter and the output.
std::string withNode(const std::string& type, const json& params = json::object(), std::string* name = nullptr) {
    GraphEdit edit(kBase);
    const std::string made = edit.addNode(type, {}, params.dump());
    edit.connect("scatter", made, "strands");
    edit.connect(made, "out", "strands");
    if (name) *name = made;
    return edit.toJson();
}

struct Evaluated {
    StrandsPtr strands;
    std::string notes;
};
Evaluated evaluate(const std::string& graph) {
    GraphDocument doc = loadGraphJson(graph);
    auto scalp = test::makeSphere(12, 24, 1.0f);
    ExternalInputs ext;
    ext.set("scalp", scalp, scalp->contentHash());
    EvalContext ctx;
    ctx.externals = &ext;
    Evaluated out;
    out.strands = doc.graph.evaluateStrands(doc.output, ctx);
    for (const auto& [node, note] : doc.graph.lastNotes()) out.notes += note + "\n";
    return out;
}

float strandLength(const StrandSet& s, std::size_t i) {
    const Vec3f* p = s.strandPoints(i);
    float l = 0.0f;
    for (std::size_t k = 1; k < s.strandPointCount(i); ++k) l += (p[k] - p[k - 1]).length();
    return l;
}

// ---- The language ----

TEST(ScriptLanguage, ErrorsSayTheLineAndColumn) {
    const ScriptError e = compileError("per point {\n    P += sin(;\n}");
    EXPECT_EQ(e.line(), 2);
    EXPECT_EQ(e.column(), 14);
    EXPECT_NE(std::string(e.what()).find("(line 2, column 14)"), std::string::npos) << e.what();
    EXPECT_EQ(e.message(), "unexpected ';'");

    struct Case {
        std::string text;
        std::string says;
        int line;
        std::vector<std::string> writes = {"P"};
    };
    const Case cases[] = {
        {"per point {\n  width = 1;\n}", "width is not in the node's \"writes\"", 2},
        {"per strand { P = root; }", "P is per point", 1},
        {"per point { P = foo(1); }", "unknown function 'foo'", 1},
        {"per point { width = P; }", "is a number, not a vector", 1, {"width"}},
        {"per point { $a += 1; }", "read before it is set", 1},
        {"per point { tangent = P; }", "input of the script (read only)", 1},
        {"per point {\n  P += N * 2\n}", "expected ';'", 3},
        {"per point { P += N;", "a block is not closed", 1},
        {"per pint { }", "expected 'strand' or 'point'", 1},
        {"", "the script is empty", 1},
        {"P += N;", "a script is made of blocks", 1},
        {"per point { } per strand { }", "the per strand block comes before", 1},
        {"per point { } per point { }", "one per point block", 1},
        {"per strand { $x = prev.P; }", "prev.P is per point", 1},
        {"per point { for $i in 0 .. 1.5 { } }", "whole numbers", 1},
        {"per point { for $i in 0 .. t { } }", "a loop's bound", 1},
        {"per point { for $i in 0 .. 200000 { } }", "at most", 1},
        {"per point { for $i in 0 .. 3 { $i = 2; } }", "loop's counter", 1},
        {"per strand { channel.m = 1; }\nper point { channel.m = 2; }", "written per strand already", 2, {"channel.m"}},
        {"per point { $v = [1, 2, 3]; $v = 1; P = $v; }", "", 0},  // a number into a vector local: on every component
        {"per point { $n = 1; $n = P; }", "is a number, not a vector", 1},
        {"per point { channel = 1; }", "input of the script", 1},
        {"per point { P = strandPoint(P); }", "a number, not a vector", 1},
        {"per point { if (P) { } }", "a number, not a vector", 1},
        {"per point { per strand { } }", "blocks don't nest", 1},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.text);
        const ScriptError error = compileError(c.text, c.writes);
        if (c.says.empty()) {
            EXPECT_EQ(error.line(), 0) << error.what();
            continue;
        }
        EXPECT_NE(std::string(error.what()).find(c.says), std::string::npos) << error.what();
        EXPECT_EQ(error.line(), c.line) << error.what();
        EXPECT_GT(error.column(), 0);
    }
    // A param is read only; a ramp is read as a call; an int bound needs a max.
    const std::vector<ScriptParam> params = {param("amount"), param("profile", ScriptParam::Kind::Ramp),
                                             param("count", ScriptParam::Kind::Int)};
    EXPECT_NE(std::string(compileError("per point { $amount = 1; }", {"P"}, params).what()).find("param of the node"), std::string::npos);
    EXPECT_NE(std::string(compileError("per point { P.x = $profile; }", {"P"}, params).what()).find("is a ramp"), std::string::npos);
    EXPECT_NE(std::string(compileError("per point { for $i in 0 .. count { } }", {"P"}, params).what()).find("with a max"),
              std::string::npos);
    EXPECT_THROW(Script("per point { P += N; }", {}, {"Q"}), ScriptError);  // writes names only outputs
}

TEST(ScriptLanguage, ExpressionsStayAsTheyWere) {
    // The expression language itself is unchanged: its locals, if/else and values.
    const auto strands = straightStrands(3, 4);
    const Expression e("$a = 2; if (t > 0.5) { $a = 3; } $a * t");
    EXPECT_FLOAT_EQ(e.evaluatePoint(*strands, 1, 3, nullptr), 3.0f);
    EXPECT_FLOAT_EQ(e.evaluatePoint(*strands, 1, 1, nullptr), 2.0f / 3.0f);
    EXPECT_THROW(Expression("per point { P = 1; }"), ExpressionError);  // no blocks in an expression
}

TEST(ScriptLanguage, APointBlockRunsOnEveryPointRootToTip) {
    const auto in = straightStrands();
    const auto out = runScript("per point { P += [1, 0, 0] * t; }", {"P"}, *in);
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        const Vec3f* a = in->strandPoints(s);
        const Vec3f* b = out->strandPoints(s);
        EXPECT_FLOAT_EQ(b[0].x, a[0].x);                 // the root stays
        EXPECT_NEAR(b[4].x, a[4].x + 1.0f, 1e-6f);       // the tip moves by 1
        EXPECT_NEAR(b[2].x, a[2].x + 0.5f, 1e-6f);
    }
    // Locals start at 0 on every strand and carry from point to point; the strand block's reach
    // the point block.
    const auto counted = runScript("per strand { $n = 10; $shift = rand; }\nper point { $n += 1; P.z = $n; P.x += $shift; }",
                                   {"P"}, *in);
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        const float shift = unitFloat(hashCombine(in->ids()[s], 0xE7A1));  // rand, as an expression's
        for (std::size_t k = 0; k < 5; ++k) {
            EXPECT_FLOAT_EQ(counted->strandPoints(s)[k].z, 11.0f + static_cast<float>(k));
            EXPECT_NEAR(counted->strandPoints(s)[k].x, in->strandPoints(s)[k].x + shift, 1e-6f);
        }
    }
    // prev.P: the point before as this run made it (the root's own at the root).
    const auto chained = runScript("per point { if (pointIndex > 0) { P = prev.P + [0, 0, 1]; } }", {"P"}, *in);
    for (std::size_t k = 0; k < 5; ++k) EXPECT_NEAR(chained->strandPoints(2)[k].z, static_cast<float>(k), 1e-6f);
    EXPECT_NEAR(chained->strandPoints(2)[4].y, 0.0f, 1e-6f);
    // A script that writes no P hands on the very points.
    const auto widths = runScript("per point { width = 0.5 * t; }", {"width"}, *in);
    EXPECT_EQ(widths->positions().data(), in->positions().data());
    EXPECT_NEAR(widths->widths()[in->strandBegin(1) + 4], 0.5f, 1e-6f);
}

TEST(ScriptLanguage, InputsReadTheStrandAsItCameIn) {
    const auto in = straightStrands(4, 5);
    const auto out = runScript(
        "per strand { channel.count = pointCount; channel.reach = distance(root, tip); }\n"
        "per point {\n"
        "    channel.index = pointIndex;\n"
        "    channel.along = t;\n"
        "    channel.up = tangent.y;\n"
        "    channel.came = pointP.y;\n"
        "    channel.mid = strandPoint(0.5).y;\n"
        "    P.y += 1;\n"
        "    channel.now = P.y;\n"
        "}",
        {"P", "channel.count", "channel.reach", "channel.index", "channel.along", "channel.up", "channel.came", "channel.mid",
         "channel.now"},
        *in);
    EXPECT_EQ((*out->strandChannel("count"))[1], 5.0f);
    EXPECT_NEAR((*out->strandChannel("reach"))[1], 1.0f, 1e-6f);
    const std::size_t p = in->strandBegin(2) + 3;
    EXPECT_EQ((*out->pointChannel("index"))[p], 3.0f);
    EXPECT_NEAR((*out->pointChannel("along"))[p], 0.75f, 1e-6f);
    EXPECT_NEAR((*out->pointChannel("up"))[p], 1.0f, 1e-6f);
    EXPECT_NEAR((*out->pointChannel("came"))[p], 0.75f, 1e-6f);
    EXPECT_NEAR((*out->pointChannel("mid"))[p], 0.5f, 1e-6f);
    EXPECT_NEAR((*out->pointChannel("now"))[p], 1.75f, 1e-6f);  // an output reads as it is now
    // Channels by name read the incoming strands' (a missing one 0).
    auto masked = std::make_shared<StrandSet>(*in);
    std::vector<float>& mask = masked->addStrandChannel("mask", 0.0f);
    for (std::size_t s = 0; s < mask.size(); ++s) mask[s] = static_cast<float>(s);
    const auto read = runScript("per point { P.x = mask + nothing; }", {"P"}, *masked);
    EXPECT_EQ(read->strandPoints(3)[2].x, 3.0f);
}

TEST(ScriptLanguage, WidthAndColorPerPointOrPerStrand) {
    const auto in = straightStrands(3, 5);
    const auto perPoint = runScript("per point { width = 0.1 + t; color = [t, 0, 1]; }", {"width", "color"}, *in);
    const std::size_t tip = in->strandBegin(1) + 4;
    EXPECT_NEAR(perPoint->widths()[tip], 1.1f, 1e-6f);
    ASSERT_TRUE(perPoint->pointChannel(kColorChannels[0]));
    EXPECT_NEAR((*perPoint->pointChannel(kColorChannels[0]))[tip], 1.0f, 1e-6f);
    EXPECT_NEAR((*perPoint->pointChannel(kColorChannels[2]))[tip], 1.0f, 1e-6f);
    // Set per strand: every point's, and what the point block reads.
    const auto perStrand = runScript("per strand { width = 0.2; color = [1, 0, 0]; }\nper point { width *= 1 + pointIndex; }",
                                     {"width", "color"}, *in);
    EXPECT_NEAR(perStrand->widths()[in->strandBegin(2)], 0.2f, 1e-6f);
    EXPECT_NEAR(perStrand->widths()[in->strandBegin(2) + 4], 1.0f, 1e-6f);
    EXPECT_EQ((*perStrand->strandChannel(kColorChannels[0]))[2], 1.0f);
    EXPECT_EQ((*perStrand->pointChannel(kColorChannels[1]))[in->strandBegin(2) + 3], 0.0f);
    // Read before written: the incoming color (the default hair color without one).
    const auto kept = runScript("per point { color = color; }", {"color"}, *in);
    EXPECT_NEAR((*kept->pointChannel(kColorChannels[0]))[3], defaultHairColor()[0], 1e-6f);
    // A channel the strands carry per strand is written per strand.
    auto channeled = std::make_shared<StrandSet>(*in);
    channeled->addStrandChannel("clumpId", 4.0f);
    EXPECT_THROW(runScript("per point { channel.clumpId = 1; }", {"channel.clumpId"}, *channeled), ScriptError);
    const auto clumped = runScript("per strand { channel.clumpId += 1; }", {"channel.clumpId"}, *channeled);
    EXPECT_EQ((*clumped->strandChannel("clumpId"))[1], 5.0f);
}

TEST(ScriptLanguage, ParamsOfEveryKind) {
    const auto in = straightStrands(2, 3);
    const std::vector<ScriptParam> params = {param("amount"), param("count", ScriptParam::Kind::Int, 5.0),
                                             param("flip", ScriptParam::Kind::Bool), param("mode", ScriptParam::Kind::Choice),
                                             param("tint", ScriptParam::Kind::Color), param("profile", ScriptParam::Kind::Ramp)};
    const Ramp ramp = Ramp::linear(2.0f, 4.0f);
    std::vector<Script::Value> values = {number(0.5f), number(100.0f), number(1.0f), number(2.0f), Script::Value{}, Script::Value{}};
    values[4].value = Vec3f{0.1f, 0.2f, 0.3f};
    values[5].ramp = &ramp;
    const auto out = runScript(
        "per point {\n"
        "    $n = 0;\n"
        "    for $i in 0 .. count { $n += 1; }\n"  // bounded by count's max (5), not its 100
        "    channel.a = $amount + $flip * 10 + $mode * 100;\n"
        "    channel.loops = $n;\n"
        "    channel.ramp = $profile(0.5);\n"
        "    channel.tint = $tint.y;\n"
        "}",
        {"channel.a", "channel.loops", "channel.ramp", "channel.tint"}, *in, params, values);
    EXPECT_NEAR((*out->pointChannel("a"))[1], 210.5f, 1e-5f);
    EXPECT_EQ((*out->pointChannel("loops"))[1], 5.0f);
    EXPECT_NEAR((*out->pointChannel("ramp"))[1], 3.0f, 1e-5f);
    EXPECT_NEAR((*out->pointChannel("tint"))[1], 0.2f, 1e-6f);
    // Loops nest; literal bounds run start to end - 1.
    const auto loops = runScript("per strand { $s = 0; for $i in 2 .. 5 { for $j in 0 .. 3 { $s += $i; } } channel.s = $s; }",
                                 {"channel.s"}, *in);
    EXPECT_EQ((*loops->strandChannel("s"))[0], 27.0f);  // (2 + 3 + 4) x 3
}

TEST(ScriptLanguage, GuidesAndTheScalp) {
    const auto in = straightStrands(2, 5);
    GuideSet guides;
    guides.add(1, {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 2.0f}});   // nearest strand 0
    guides.add(2, {{0.5f, 0.0f, 0.0f}, {0.5f, 3.0f, 0.0f}});   // nearest strand 1
    const auto out = runScript("per point { P = guidePoint(t); }", {"P"}, *in, {}, {}, &guides);
    EXPECT_NEAR(out->strandPoints(0)[4].z, 2.0f, 1e-6f);
    EXPECT_NEAR(out->strandPoints(1)[4].y, 3.0f, 1e-6f);
    // Without guides: the strand's own point.
    const auto own = runScript("per point { P = guidePoint(t); }", {"P"}, *in);
    EXPECT_EQ(own->positions(), in->positions());
    // The scalp: the closest point and its normal (a unit sphere: p's direction).
    const auto sphere = test::makeSphere(24, 48, 1.0f);
    const auto onScalp = runScript("per point { channel.d = length(closestOnScalp([0, 3, 0])); channel.n = scalpNormal([2, 0, 0]).x; }",
                                   {"channel.d", "channel.n"}, *in, {}, {}, nullptr, sphere.get());
    EXPECT_NEAR((*onScalp->pointChannel("d"))[0], 1.0f, 1e-3f);
    EXPECT_NEAR((*onScalp->pointChannel("n"))[0], 1.0f, 1e-2f);
}

TEST(ScriptLanguage, ARunawayScriptFailsFastAndNeverHangs) {
    const auto in = straightStrands(200, 8);
    const auto start = std::chrono::steady_clock::now();
    try {
        runScript("per point { for $i in 0 .. 100000 { for $j in 0 .. 100000 { P.x += 1; } } }", {"P"}, *in);
        FAIL() << "a script past its budget fails";
    } catch (const ScriptError& e) {
        EXPECT_NE(std::string(e.what()).find("budget"), std::string::npos) << e.what();
    }
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 2.0);
    // Within the budget: fine.
    EXPECT_NO_THROW(runScript("per point { for $i in 0 .. 500 { P.x += 0.001; } }", {"P"}, *in));
}

TEST(ScriptLanguage, TheSameOnAnyThreadCount) {
    const auto in = straightStrands(3000, 12);
    const std::string text =
        "per strand { $phase = rand * 6.28; }\n"
        "per point { P += vfbm(P * 3 + $phase) * t; width = 0.01 * (1 + noise(P)); }";
    const auto many = runScript(text, {"P", "width"}, *in);
    struct Context {
        const std::string* text;
        const StrandSet* in;
        std::shared_ptr<StrandSet> out;
    } context{&text, in.get(), nullptr};
    runWithThreads(1, [](void* c) {
        auto& ctx = *static_cast<Context*>(c);
        ctx.out = runScript(*ctx.text, {"P", "width"}, *ctx.in);
    }, &context);
    ASSERT_TRUE(context.out);
    EXPECT_EQ(context.out->positions(), many->positions());
    EXPECT_EQ(context.out->widths(), many->widths());
}

// ---- The node ----

// ---- Files and the library ----

}  // namespace
}  // namespace groomlab
