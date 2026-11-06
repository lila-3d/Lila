// SPDX-License-Identifier: Apache-2.0
// Script nodes: a compiled script run over strands (script.h). The language's parser and
// interpreter live with the expression language's (expression.cpp, script_program.h); this is the
// driver: the inputs set before each block, the outputs written after, strands side by side.

#include "groomlab/script.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "groomlab/export_view.h"
#include "groomlab/field.h"
#include "groomlab/guide_set.h"
#include "groomlab/hair_color.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/preview_core.h"
#include "groomlab/strand_set.h"
#include "script_program.h"

namespace groomlab {

struct Script::Impl {
    script::Program program;
    std::vector<ScriptParam> params;
};

namespace {

// Each point's arc share along the polyline, 0 at the first to 1 at the last (by index when it has
// no length), as Expression's t.
void arcShares(const Vec3f* pts, std::size_t n, std::vector<float>& out) {
    out.resize(n);
    float total = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) total += (pts[i] - pts[i - 1]).length();
        out[i] = total;
    }
    if (total > 1e-12f) {
        for (float& a : out) a /= total;
    } else {
        for (std::size_t i = 0; i < n; ++i) out[i] = n > 1 ? float(i) / float(n - 1) : 0.0f;
    }
}

// The strand's direction at point i (both neighbours; one at the ends), unit length.
Vec3f tangentAt(const Vec3f* pts, std::size_t n, std::size_t i) {
    if (n < 2) return {0.0f, 1.0f, 0.0f};
    const Vec3f d = i == 0 ? pts[1] - pts[0] : (i + 1 == n ? pts[n - 1] - pts[n - 2] : pts[i + 1] - pts[i - 1]);
    const float l = d.length();
    return l > 0.0f ? d * (1.0f / l) : Vec3f{0.0f, 1.0f, 0.0f};
}

void putSlots(float* slots, std::uint16_t at, const Vec3f& v) {
    slots[at] = v.x;
    slots[at + 1] = v.y;
    slots[at + 2] = v.z;
}
Vec3f slotVector(const float* slots, std::uint16_t at) { return {slots[at], slots[at + 1], slots[at + 2]}; }

}  // namespace

ScriptError::ScriptError(const std::string& message, int line, int column)
    : ExpressionError(line > 0 ? message + " (line " + std::to_string(line) + ", column " + std::to_string(column) + ")" : message),
      message_(message),
      line_(line),
      column_(column) {}

Script::Script(const std::string& text, const std::vector<ScriptParam>& params, const std::vector<std::string>& writes) {
    for (const std::string& w : writes) {
        const bool channel = w.rfind("channel.", 0) == 0 && w.size() > 8;
        if (w != "P" && w != "width" && w != "color" && !channel) {
            throw ScriptError("\"writes\" lists P, width, color and channel.name, not '" + w + "'", 0, 0);
        }
    }
    std::vector<script::ParamInfo> infos;
    for (const ScriptParam& p : params) {
        script::ParamInfo info;
        info.name = p.name;
        info.width = p.kind == ScriptParam::Kind::Color ? 3 : 1;
        info.ramp = p.kind == ScriptParam::Kind::Ramp;
        info.loopBound = p.kind == ScriptParam::Kind::Int && p.max.has_value();
        info.loopMax = p.max ? static_cast<float>(std::min(*p.max, static_cast<double>(kScriptMaxLoop))) : 0.0f;
        infos.push_back(info);
    }
    auto impl = std::make_shared<Impl>();
    impl->program = script::compile(text, infos, writes);
    impl->params = params;
    impl_ = std::move(impl);
}

bool Script::writesPoints() const { return impl_ && impl_->program.pointP; }
bool Script::writesWidths() const { return impl_ && (impl_->program.strandWidth || impl_->program.pointWidth); }
bool Script::writesColors() const { return impl_ && (impl_->program.strandColor || impl_->program.pointColor); }
std::vector<std::string> Script::strandChannels() const {
    std::vector<std::string> out;
    if (!impl_) return out;
    for (const script::ChannelOut& c : impl_->program.channelOuts) {
        if (c.assigned && !c.perPoint) out.push_back(c.name);
    }
    return out;
}
std::vector<std::string> Script::pointChannels() const {
    std::vector<std::string> out;
    if (!impl_) return out;
    for (const script::ChannelOut& c : impl_->program.channelOuts) {
        if (c.assigned && c.perPoint) out.push_back(c.name);
    }
    return out;
}
bool Script::readsGuides() const { return impl_ && impl_->program.readsGuides; }
bool Script::readsTime() const { return impl_ && impl_->program.readsTime; }
bool Script::readsLongest() const { return impl_ && impl_->program.readsLongest; }
std::vector<std::string> Script::channelsRead() const { return impl_ ? impl_->program.channels : std::vector<std::string>{}; }

void Script::run(StrandSet& out, const StrandSet& in, const Inputs& inputs, const std::vector<std::uint32_t>* only) const {
    if (!impl_) return;
    const script::Program& program = impl_->program;
    const std::size_t count = in.strandCount();
    if (out.strandCount() != count || out.pointCount() != in.pointCount()) throw ScriptError("the strands do not match", 0, 0);
    if (inputs.params.size() != impl_->params.size()) throw ScriptError("a value for every param is needed", 0, 0);

    // The channels it writes: of the kind the strands coming in have them, if they have them.
    for (const script::ChannelOut& c : program.channelOuts) {
        if (!c.assigned) continue;
        if (c.perPoint && in.strandChannel(c.name)) {
            throw ScriptError("channel." + c.name + " is a strand channel on the strands coming in: write it in the per strand block", 0, 0);
        }
        if (!c.perPoint && in.pointChannel(c.name)) {
            throw ScriptError("channel." + c.name + " is a point channel on the strands coming in: write it in the per point block", 0, 0);
        }
    }

    // The arrays it writes, each taken for writing once, before the strands run side by side.
    std::vector<Vec3f>* positions = program.pointP ? &out.mutablePositions() : nullptr;
    const bool widthOut = program.strandWidth || program.pointWidth;
    const bool colorOut = program.strandColor || program.pointColor;
    std::vector<float>* widths = nullptr;
    if (widthOut) {
        if (out.widths().size() != out.pointCount()) out.overwriteWidths().assign(out.pointCount(), 1.0f);
        widths = &out.mutableWidths();
    }
    const std::array<float, 3> defaultColor = defaultHairColor();
    std::array<std::vector<float>*, 3> pointColor{}, strandColor{};
    if (colorOut) {
        for (int k = 0; k < 3; ++k) {
            const std::vector<float>* had = out.pointChannel(kColorChannels[k]);
            pointColor[k] = had && had->size() == out.pointCount() ? out.mutablePointChannel(kColorChannels[k])
                                                                   : &out.addPointChannel(kColorChannels[k], defaultColor[k]);
            const std::vector<float>* hadStrand = out.strandChannel(kColorChannels[k]);
            strandColor[k] = hadStrand && hadStrand->size() == count ? out.mutableStrandChannel(kColorChannels[k])
                                                                     : &out.addStrandChannel(kColorChannels[k], defaultColor[k]);
        }
    }
    struct ChannelArrays {
        const script::ChannelOut* out = nullptr;
        const std::vector<float>* in = nullptr;  // its incoming values (null: 0)
        std::vector<float>* values = nullptr;
    };
    std::vector<ChannelArrays> channels;
    for (const script::ChannelOut& c : program.channelOuts) {
        if (!c.assigned) continue;
        ChannelArrays a;
        a.out = &c;
        if (c.perPoint) {
            a.in = in.pointChannel(c.name);
            const std::vector<float>* had = out.pointChannel(c.name);
            a.values = had && had->size() == out.pointCount() ? out.mutablePointChannel(c.name) : &out.addPointChannel(c.name, 0.0f);
        } else {
            a.in = in.strandChannel(c.name);
            const std::vector<float>* had = out.strandChannel(c.name);
            a.values = had && had->size() == count ? out.mutableStrandChannel(c.name) : &out.addStrandChannel(c.name, 0.0f);
        }
        channels.push_back(a);
    }

    // What the strands bring: widths, colors (HairColor's per point, else per strand, else the
    // pigment's or the default), channels the script reads by name.
    const std::vector<float>& inWidths = in.widths();
    const bool hasWidths = inWidths.size() == in.pointCount();
    std::array<const std::vector<float>*, 3> inPointColor{}, inStrandColor{};
    bool hasPointColors = true, hasStrandColors = true;
    for (int k = 0; k < 3; ++k) {
        inPointColor[k] = in.pointChannel(kColorChannels[k]);
        inStrandColor[k] = in.strandChannel(kColorChannels[k]);
        hasPointColors = hasPointColors && inPointColor[k] && inPointColor[k]->size() == in.pointCount();
        hasStrandColors = hasStrandColors && inStrandColor[k] && inStrandColor[k]->size() == count;
    }
    std::vector<std::array<float, 3>> pigment;  // strandColors(): from the melanin channels
    if (!hasPointColors && !hasStrandColors) pigment = strandColors(in);
    auto colorIn = [&](std::size_t s, std::size_t p) -> Vec3f {
        if (hasPointColors) return {(*inPointColor[0])[p], (*inPointColor[1])[p], (*inPointColor[2])[p]};
        if (hasStrandColors) return {(*inStrandColor[0])[s], (*inStrandColor[1])[s], (*inStrandColor[2])[s]};
        if (s < pigment.size()) return {pigment[s][0], pigment[s][1], pigment[s][2]};
        return {defaultColor[0], defaultColor[1], defaultColor[2]};
    };
    std::vector<const std::vector<float>*> bound, boundPoint;
    script::bindChannels(program, in, &bound, &boundPoint);
    float longest = 0.0f;
    if (program.readsLongest) {  // of the core strands (preview_core.h), as an expression's
        const PreviewCore core(in);
        for (std::size_t s = 0; s < count; ++s) {
            if (!core.core(s)) continue;
            const Vec3f* pts = in.strandPoints(s);
            float length = 0.0f;
            for (std::size_t i = 1, n = in.strandPointCount(s); i < n; ++i) length += (pts[i] - pts[i - 1]).length();
            longest = std::max(longest, length);
        }
    }

    // Each strand's guide: the guide rooted nearest its root.
    std::vector<std::vector<float>> guideArcs;
    PointIndex guideRoots;
    const std::vector<Guide>* guides = nullptr;
    if (program.readsGuides && inputs.guides && !inputs.guides->empty()) {
        guides = &inputs.guides->guides();
        std::vector<Vec3f> roots;
        for (const Guide& g : *guides) {
            roots.push_back(g.points.empty() ? Vec3f{} : g.points.front());
            guideArcs.emplace_back();
            arcShares(g.points.data(), g.points.size(), guideArcs.back());
        }
        guideRoots = PointIndex(std::move(roots));
    }

    // The params: their values (a mappable one's per strand or point), the ramps.
    std::vector<const Ramp*> ramps;
    for (const Value& v : inputs.params) {
        if (v.ramp) ramps.push_back(v.ramp);
    }
    const std::size_t paramCount = impl_->params.size();

    std::atomic<bool> failed{false};
    std::mutex errorLock;
    std::string error;
    auto fail = [&](const char* block) {
        std::lock_guard<std::mutex> lock(errorLock);
        if (failed.exchange(true)) return;
        error = std::string("the per ") + block + " block ran past its budget of " + std::to_string(kScriptStepBudget) +
                " steps for one " + block + ": a loop runs too long";
    };

    auto strand = [&](std::size_t s) {
        if (failed.load(std::memory_order_relaxed)) return;
        const std::size_t n = in.strandPointCount(s);
        if (n == 0) return;
        const Vec3f* pts = in.strandPoints(s);
        const std::size_t begin = in.strandBegin(s);
        thread_local std::vector<float> slots, arc;
        thread_local std::vector<unsigned char> dirty;
        slots.assign(program.slotCount, 0.0f);
        dirty.assign(program.slotCount, 0);
        arcShares(pts, n, arc);

        script::Frame f;
        script::strandVariables(f.vars, program, in, s, inputs.scalp, longest);
        f.strands = &in;
        f.strand = s;
        f.point = begin;
        f.channels = &bound;
        f.pointChannels = &boundPoint;
        f.root = s < in.roots().size() ? &in.roots()[s] : nullptr;
        f.slots = slots.data();
        f.dirty = dirty.data();
        f.points = pts;
        f.arc = arc.data();
        f.pointCount = n;
        f.scalp = inputs.scalp;
        f.ramps = ramps.data();
        if (guides) {
            const std::size_t g = guideRoots.nearest(pts[0]);
            if (g < guides->size() && !(*guides)[g].points.empty()) {
                f.guidePoints = (*guides)[g].points.data();
                f.guideArc = guideArcs[g].data();
                f.guideCount = (*guides)[g].points.size();
            }
        }

        // The strand's own slots, then its root point's for the strand block.
        putSlots(f.slots, script::kSlotRoot, pts[0]);
        putSlots(f.slots, script::kSlotTip, pts[n - 1]);
        f.slots[script::kSlotPointCount] = static_cast<float>(n);
        for (std::size_t i = 0; i < paramCount; ++i) {
            const Value& v = inputs.params[i];
            if (v.ramp) continue;
            const std::uint16_t at = program.paramSlots[i];
            if (impl_->params[i].kind == ScriptParam::Kind::Color) {
                putSlots(f.slots, at, v.value);
            } else {
                f.slots[at] = v.field ? v.field->at(in, s) : v.value.x;
            }
        }
        for (const ChannelArrays& c : channels) {
            if (!c.out->perPoint) f.slots[c.out->slot] = c.in && s < c.in->size() ? (*c.in)[s] : 0.0f;
        }
        putSlots(f.slots, script::kSlotP, pts[0]);
        f.slots[script::kSlotWidth] = hasWidths ? inWidths[begin] : 1.0f;
        putSlots(f.slots, script::kSlotColor, colorIn(s, begin));
        putSlots(f.slots, script::kSlotTangent, tangentAt(pts, n, 0));
        putSlots(f.slots, script::kSlotPrevP, pts[0]);
        if (program.hasStrandBlock && !script::run(program.strandOps, program, f, kScriptStepBudget)) {
            fail("strand");
            return;
        }
        // What the strand block set for every point.
        const bool widthSet = dirty[script::kSlotWidth] != 0;
        const float widthValue = f.slots[script::kSlotWidth];
        const bool colorSet = dirty[script::kSlotColor] || dirty[script::kSlotColor + 1] || dirty[script::kSlotColor + 2];
        const Vec3f colorValue = slotVector(f.slots, script::kSlotColor);
        for (const ChannelArrays& c : channels) {
            if (!c.out->perPoint) (*c.values)[s] = f.slots[c.out->slot];
        }
        if (colorOut) {
            for (int k = 0; k < 3; ++k) (*strandColor[k])[s] = std::clamp((&colorValue.x)[k], 0.0f, 1.0f);
        }

        // The points, root to tip (also to lay a strand block's width or color on them).
        if (!program.hasPointBlock && !widthOut && !colorOut) return;
        Vec3f prev = pts[0];
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t p = begin + i;
            const float width = widthSet ? widthValue : (hasWidths ? inWidths[p] : 1.0f);
            const Vec3f color = colorSet ? colorValue : colorIn(s, p);
            if (program.hasPointBlock) {
                putSlots(f.slots, script::kSlotP, pts[i]);
                f.slots[script::kSlotWidth] = width;
                putSlots(f.slots, script::kSlotColor, color);
                putSlots(f.slots, script::kSlotTangent, tangentAt(pts, n, i));
                putSlots(f.slots, script::kSlotPrevP, prev);
                for (const ChannelArrays& c : channels) {
                    if (c.out->perPoint) f.slots[c.out->slot] = c.in && p < c.in->size() ? (*c.in)[p] : 0.0f;
                }
                for (std::size_t k = 0; k < paramCount; ++k) {
                    const Value& v = inputs.params[k];
                    if (v.field && v.field->perPoint()) f.slots[program.paramSlots[k]] = v.field->atPoint(in, s, i);
                }
                script::pointVariables(f.vars, arc[i], i, n, pts[i]);
                f.point = p;
                if (!script::run(program.pointOps, program, f, kScriptStepBudget)) {
                    fail("point");
                    return;
                }
                const Vec3f made = slotVector(f.slots, script::kSlotP);
                if (positions) (*positions)[p] = made;
                prev = positions ? made : pts[i];
                if (widths) (*widths)[p] = std::max(f.slots[script::kSlotWidth], 0.0f);
                if (colorOut) {
                    for (int k = 0; k < 3; ++k) (*pointColor[k])[p] = std::clamp(f.slots[script::kSlotColor + k], 0.0f, 1.0f);
                }
                for (const ChannelArrays& c : channels) {
                    if (c.out->perPoint) (*c.values)[p] = f.slots[c.out->slot];
                }
            } else {
                if (widths) (*widths)[p] = std::max(width, 0.0f);
                if (colorOut) {
                    for (int k = 0; k < 3; ++k) (*pointColor[k])[p] = std::clamp((&color.x)[k], 0.0f, 1.0f);
                }
            }
        }
    };
    if (only) {
        parallelFor(0, only->size(), [&](std::size_t k) { strand(static_cast<std::size_t>((*only)[k])); }, 64);
    } else {
        parallelFor(0, count, strand, 64);
    }
    if (failed) throw ScriptError(error, 0, 0);
}

}  // namespace groomlab
