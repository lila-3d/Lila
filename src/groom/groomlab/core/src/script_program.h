// SPDX-License-Identifier: Apache-2.0
#pragma once

// Script nodes' programs (script.h): what the expression language's parser and interpreter
// (expression.cpp) compile a script into and run, and what the node's driver (script.cpp) fills
// in around each run. Private to the core.
//
// A program is two lists of expression ops (Expression::Op), one per block, with five kinds of
// op of their own: Load and Store of slots, Jump, JumpIfZero and ScriptCall. Slots are floats a
// strand's run keeps from its first block to its last point: the built-in inputs and outputs at
// fixed places (below), then the params, the channels the script writes and its locals. The
// driver sets the inputs before each block and reads the outputs after; locals start at 0 on
// every strand and keep their values from one point to the next.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/expression.h"
#include "groomlab/ramp.h"

namespace groomlab {

class ScalpMesh;
class StrandSet;
struct StrandRoot;

namespace script {

// The built-in slots.
constexpr std::uint16_t kSlotP = 0;           // the point (a vector; per strand: the root point)
constexpr std::uint16_t kSlotWidth = 3;       // the point's width (per strand: the root's)
constexpr std::uint16_t kSlotColor = 4;       // its color (a vector)
constexpr std::uint16_t kSlotTangent = 7;     // the strand's direction there, as it came in
constexpr std::uint16_t kSlotPrevP = 10;      // the point before, as this run made it
constexpr std::uint16_t kSlotRoot = 13;       // the strand's first point
constexpr std::uint16_t kSlotTip = 16;        // its last point
constexpr std::uint16_t kSlotPointCount = 19; // its points
constexpr std::uint16_t kFirstFreeSlot = 20;

// Script functions (ScriptCall's code).
enum Function : std::uint8_t { kStrandPoint, kGuidePoint, kScalpNormal, kClosestOnScalp, kRampParam };

// A header param as the compiler needs it.
struct ParamInfo {
    std::string name;
    int width = 1;          // slots: 3 for a color
    bool ramp = false;      // read as $name(x), from Frame::ramps
    bool loopBound = false; // a whole number with a max: may bound a for loop
    float loopMax = 0.0f;
};

// A channel the script writes: per strand (written in the per strand block) or per point.
struct ChannelOut {
    std::string name;
    std::uint16_t slot = 0;
    bool perPoint = false;
    bool assigned = false;  // the script assigns it (else listed in writes only)
};

struct Program {
    std::vector<Expression::Op> strandOps, pointOps;
    bool hasStrandBlock = false, hasPointBlock = false;
    // The tables the ops name, as Expression's.
    std::vector<std::string> channels;  // read by name (bound to the strands per run)
    std::vector<Ramp> ramps;
    std::vector<ExpressionCurve> curves;
    std::vector<std::string> mapNames;
    std::vector<ExpressionMapPtr> maps;
    double seconds = 0.0, frame = 0.0;  // the environment's when compiled
    // The slots: each param's first (a ramp's: its index among the ramp params), the channels
    // written, how many in all.
    std::vector<std::uint16_t> paramSlots;
    std::vector<ChannelOut> channelOuts;
    std::size_t slotCount = kFirstFreeSlot;
    // What it writes, by block.
    bool pointP = false;                       // P (per point only)
    bool strandWidth = false, pointWidth = false;
    bool strandColor = false, pointColor = false;
    // What it reads beyond the strand.
    bool readsTime = false, readsLongest = false, readsGuides = false, readsScalp = false;
};

// Compiles `text` for a node with these params, allowed to write `writes` ("P", "width",
// "color", "channel.<name>"). Throws ScriptError (script.h) with the line and column. Reads the
// thread's ExpressionEnvironment (time, maps), as Expression does.
Program compile(const std::string& text, const std::vector<ParamInfo>& params, const std::vector<std::string>& writes);

// What one block's run reads and writes.
struct Frame {
    float vars[Expression::kVariables] = {};
    const StrandSet* strands = nullptr;  // the strands coming in (channels, map() roots)
    std::size_t strand = 0;
    std::size_t point = 0;  // into the point arrays
    const std::vector<const std::vector<float>*>* channels = nullptr;       // Program::channels bound
    const std::vector<const std::vector<float>*>* pointChannels = nullptr;
    const StrandRoot* root = nullptr;
    float* slots = nullptr;
    unsigned char* dirty = nullptr;  // set for each slot a Store writes
    // The script functions' data: the strand as it came in (its points and their arc share 0-1),
    // its guide (none: guidePoint gives the strand's own point), the scalp, the ramp params.
    const Vec3f* points = nullptr;
    const float* arc = nullptr;
    std::size_t pointCount = 0;
    const Vec3f* guidePoints = nullptr;
    const float* guideArc = nullptr;
    std::size_t guideCount = 0;
    const ScalpMesh* scalp = nullptr;
    const Ramp* const* ramps = nullptr;
};

// Runs a block's ops; false when it took more than `budget` steps (it stops there).
bool run(const std::vector<Expression::Op>& ops, const Program& program, Frame& frame, std::size_t budget);

// The strand's variables (every one: root, UV, rand, id, shape...), point variables at its root.
void strandVariables(float* vars, const Program& program, const StrandSet& strands, std::size_t s, const ScalpMesh* scalp,
                     float longest);
// The point variables of point `index` of n: t (arc share), pt, px/py/pz, pointIndex.
void pointVariables(float* vars, float t, std::size_t index, std::size_t n, const Vec3f& p);
// Binds the program's channel names to the strands (as Expression::bind).
void bindChannels(const Program& program, const StrandSet& strands, std::vector<const std::vector<float>*>* channels,
                  std::vector<const std::vector<float>*>* pointChannels);

// The point at arc share t (0-1) of a polyline whose cumulative arc shares are `arc`.
Vec3f pointAlong(const Vec3f* points, const float* arc, std::size_t n, float t);

}  // namespace script
}  // namespace groomlab
