// SPDX-License-Identifier: Apache-2.0
// Group: a named set of strands, as a 0-1 strand channel other nodes' "affect" masks read by name
// (Yeti's Group node and per-node group restriction): inside a sphere or a box, where a map (or an
// expression bound to its value) passes a threshold, or near the guides of hierarchy groups.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <set>

#include "groomlab/field.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operators.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "internal.h"

namespace groomlab {

namespace {

// Names separated by spaces or commas.
std::vector<std::string> namesIn(const std::string& text) {
    std::vector<std::string> out;
    std::string name;
    for (const char c : text + " ") {
        if (c == ' ' || c == ',' || c == '\t') {
            if (!name.empty()) out.push_back(name);
            name.clear();
        } else {
            name += c;
        }
    }
    return out;
}

// 1 at `distance` 0 and inside (negative), falling smoothly to 0 at `falloff` out; a hard edge at 0.
float edge(float distance, float falloff) {
    if (distance <= 0.0f) return 1.0f;
    if (falloff <= 0.0f) return 0.0f;
    return 1.0f - smoothstep(0.0f, falloff, distance);
}

// A value against a threshold: 1 at or above, 0 below; with softness, a smooth step that wide
// around it.
float pass(float value, float threshold, float softness) {
    if (softness <= 0.0f) return value >= threshold ? 1.0f : 0.0f;
    return smoothstep(threshold - 0.5f * softness, threshold + 0.5f * softness, value);
}

class GroupOp : public Operator {
public:
    std::string typeName() const override { return op::kGroup; }
    // Its group, added to one of its name made before unless it replaces it (combine).
    std::vector<ChannelWrite> channelWrites() const override {
        std::vector<ChannelWrite> out = Operator::channelWrites();
        for (ChannelWrite& w : out) w.extends = params().getString("combine") != "replace";
        return out;
    }
    std::string category() const override { return "utility"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Curves}}; }
    PortSpec output() const override { return {"out", PortType::Curves}; }  // strands or guides, as fed
    std::vector<ParamSpec> paramSpecs() const override {
        const std::vector<std::string> region = {"sphere", "box"};
        const std::vector<std::string> valued = {"map", "guideGroup"};
        return {
            ParamSpec{"name", std::string("group1"),
                      "The group's name: the strand channel it writes (1 in the group, 0 out, between on soft edges), "
                      "which an affect map, an expression or a Split reads"}
                .writesChannel(),
            {"by", std::string("sphere"),
             "What makes the group: the roots inside a sphere or a box; where the value (a map, or an expression bound "
             "to it) passes the threshold; or near the guides of hierarchy groups",
             "", {"sphere", "box", "map", "guideGroup"}},
            ParamSpec{"center", Vec3f{0.0f, 0.0f, 0.0f}, "The sphere's or box's center (world space)", "length"}.when("by", region),
            ParamSpec{"radius", 1.0, "The sphere's radius", "length"}.atLeast(0).soft(0, 10).when("by", {"sphere"}),
            ParamSpec{"size", Vec3f{1.0f, 1.0f, 1.0f}, "The box's size along x, y and z (world space)", "length"}.when("by", {"box"}),
            ParamSpec{"falloff", 0.0, "A soft edge this wide outside the sphere or box (0: hard)", "length"}
                .atLeast(0)
                .soft(0, 2)
                .when("by", region),
            ParamSpec{"at", std::string("root"), "Which point of a strand is tested against the sphere or box", "",
                      {"root", "tip"}}
                .when("by", region),
            ParamSpec{"value", 1.0, "The value tested against the threshold, per strand (mappable: a painted map, a "
                                    "texture, a channel or an expression such as $y > 0)", "fraction"}
                .soft(0, 1)
                .when("by", {"map"}),
            ParamSpec{"guideGroup", std::string(),
                      "Hierarchy groups (space or comma separated): the strands nearest their guides (and their child "
                      "groups'), blended from the 4 nearest guides"}
                .when("by", {"guideGroup"}),
            ParamSpec{"threshold", 0.5, "In the group from this value up", "fraction"}.soft(0, 1).when("by", valued),
            ParamSpec{"softness", 0.0, "A smooth step this wide around the threshold instead of a hard one", "fraction"}
                .range(0, 1)
                .when("by", valued),
            {"invert", false, "Everything but the region"},
            {"combine", std::string("replace"),
             "With a group of the same name made before: replace it, add to it (the larger), subtract from it, or "
             "keep where both are (the smaller)",
             "", {"replace", "add", "subtract", "intersect"}},
        };
    }
    std::vector<std::string> externalInputs() const override {
        if (params().getString("by") == "guideGroup") return {"scalp", "hierarchy"};
        return {"scalp"};
    }

    Value evaluate(const EvalContext& ctx, const std::vector<Value>& inputs) const override {
        const StrandsPtr in = requireStrands(inputs, "Group");
        const std::string name = params().getString("name");
        const bool named = !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') &&
                           std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        if (!named) throw GraphError("Group: '" + name + "' is not a group name (letters, digits and _)");
        const ScalpMesh* scalp = scalpOf(ctx);
        const std::string by = params().getString("by");
        const bool invert = params().getBool("invert");
        const std::string combine = params().getString("combine");
        const std::size_t count = in->strandCount();
        std::vector<float> member(count, 0.0f);

        auto rootOf = [&](std::size_t s) {
            const std::size_t n = in->strandPointCount(s);
            return n ? in->strandPoints(s)[0] : Vec3f{};
        };
        if (by == "sphere" || by == "box") {
            const Vec3f center = params().getVec3("center");
            const bool tip = params().getString("at") == "tip";
            const float falloff = static_cast<float>(params().getDouble("falloff"));
            const float radius = static_cast<float>(params().getDouble("radius"));
            const Vec3f half = params().getVec3("size") * 0.5f;
            const bool sphere = by == "sphere";
            parallelFor(0, count, [&](std::size_t s) {
                const std::size_t n = in->strandPointCount(s);
                if (n == 0) return;
                const Vec3f p = (tip ? in->strandPoints(s)[n - 1] : in->strandPoints(s)[0]) - center;
                float distance;  // outside the region (0 or less: inside)
                if (sphere) {
                    distance = p.length() - radius;
                } else {
                    const Vec3f q{std::abs(p.x) - std::abs(half.x), std::abs(p.y) - std::abs(half.y), std::abs(p.z) - std::abs(half.z)};
                    const Vec3f outside{std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
                    distance = outside.length() + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
                }
                member[s] = edge(distance, falloff);
            });
        } else if (by == "map") {
            const ScalarField field(params(), "value", scalp);
            field.validate(*in);
            const float threshold = static_cast<float>(params().getDouble("threshold"));
            const float softness = static_cast<float>(params().getDouble("softness"));
            parallelFor(0, count, [&](std::size_t s) { member[s] = pass(field.at(*in, s), threshold, softness); });
        } else if (by == "guideGroup") {
            const HierarchyPtr hierarchy = valueAs<HierarchyPtr>(ctx.external("hierarchy"));
            const std::vector<std::string> groupNames = namesIn(params().getString("guideGroup"));
            if (groupNames.empty()) throw GraphError("Group: name the hierarchy groups (guideGroup)");
            if (!hierarchy || hierarchy->guides().empty()) throw GraphError("Group: the groom has no guide hierarchy");
            std::set<GroupId> wanted;
            for (const std::string& g : groupNames) {
                const GroupId id = hierarchy->findGroup(g);
                if (id == kNoGroup) throw GraphError("Group: no guide group named '" + g + "'");
                wanted.insert(id);
            }
            // A guide is in when its group, or a group above it, is one named.
            auto inWanted = [&](GroupId group) {
                for (int depth = 0; group != kNoGroup && depth < 1000; ++depth) {
                    if (wanted.count(group)) return true;
                    const GuideGroup* g = hierarchy->group(group);
                    group = g ? g->parent : kNoGroup;
                }
                return false;
            };
            std::vector<Vec3f> roots;
            std::vector<float> in1;
            for (const HierarchyGuide& g : hierarchy->guides()) {
                if (g.points.empty()) continue;
                roots.push_back(g.points.front());
                in1.push_back(inWanted(g.group) ? 1.0f : 0.0f);
            }
            if (roots.empty()) throw GraphError("Group: the guide hierarchy has no guides with points");
            const PointIndex index(roots);
            const float threshold = static_cast<float>(params().getDouble("threshold"));
            const float softness = static_cast<float>(params().getDouble("softness"));
            parallelFor(0, count, [&](std::size_t s) {
                std::array<std::size_t, 4> idx{};
                std::array<float, 4> d2{};
                const std::size_t found = index.kNearest(rootOf(s), idx.size(), idx.data(), d2.data());
                float sum = 0.0f, weights = 0.0f;
                for (std::size_t i = 0; i < found; ++i) {
                    if (d2[i] <= 1e-12f) {
                        sum = in1[idx[i]];
                        weights = 1.0f;
                        break;
                    }
                    const float w = 1.0f / d2[i];
                    sum += in1[idx[i]] * w;
                    weights += w;
                }
                member[s] = pass(weights > 0.0f ? sum / weights : 0.0f, threshold, softness);
            });
        } else {
            throw GraphError("Group: unknown 'by' '" + by + "' (sphere, box, map or guideGroup)");
        }

        auto out = std::make_shared<StrandSet>(*in);
        const std::vector<float>* before = in->strandChannel(name);
        std::vector<float>& channel = before ? *out->mutableStrandChannel(name) : out->addStrandChannel(name, 0.0f);
        for (std::size_t s = 0; s < count; ++s) {
            float g = std::clamp(member[s], 0.0f, 1.0f);
            if (invert) g = 1.0f - g;
            const float old = before ? (*before)[s] : 0.0f;
            if (combine == "add") g = std::max(old, g);
            else if (combine == "subtract") g = old * (1.0f - g);
            else if (combine == "intersect") g = std::min(old, g);
            channel[s] = g;
        }
        return StrandsPtr(out);
    }
};

}  // namespace

void registerGroupOperator(OperatorRegistry& registry) {
    registry.registerType(op::kGroup,
                          {"utility",
                           "Names a set of strands (a region, a map, an expression, guide groups) for other nodes' affect to restrict them to.",
                           "region mask selection set"},
                          [] { return std::make_unique<GroupOp>(); });
}

}  // namespace groomlab
