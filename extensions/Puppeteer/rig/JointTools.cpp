// Joint tools for Rig mode. Most are the base rigging commands under Puppeteer's menu and hotkeys; mirroring works out
// the side naming by itself.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/UI.h>

using namespace lila;
using namespace lila::scene;

namespace puppeteer
{
namespace
{
/** The search/replace pair that names the opposite side of name, or nothing for a centre joint. */
std::optional<std::pair<std::string, std::string>> sidePair(const std::string& name)
{
    static const std::pair<const char*, const char*> pairs[] = {{"_l", "_r"}, {"_L", "_R"}, {"Left", "Right"}, {"left", "right"},
                                                                {"l_", "r_"}, {"L_", "R_"}};
    for (const auto& [l, r] : pairs)
        for (const auto& [s, d] : {std::pair<std::string, std::string>{l, r}, {r, l}})
            if (rig::mirrorName(name, s, d) != name)
                return std::pair{s, d};
    return std::nullopt;
}

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "puppeteerMirrorJoints",
        .doc = "Mirror joints (and everything below them) to the other side, naming the copies by the side the joint's "
               "own name gives (arm_l to arm_r, LeftArm to RightArm, l_arm to r_arm). Returns the new roots.",
        .positionalDoc = "joints (default the selection)",
        .flags = {
            {"axis", "ax", T::String, "x", "Mirror across this axis: x mirrors across the YZ plane.", {"x", "y", "z"}},
            {"behavior", "b", T::Bool, true, "Mirror behaviour instead of orientation."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (const std::string& s : objectArgs(a))
            {
                const Node& j = jointNamed(g, s, "puppeteerMirrorJoints");
                const auto pair = sidePair(j.name());
                if (!pair)
                    throw CommandError("puppeteerMirrorJoints: " + j.name() + " has no side in its name");
                json r = ops::execute("mirrorJoints", {{"args", {j.name()}},
                                                        {"axis", a.get<std::string>("axis", "x")},
                                                        {"behavior", a.get<bool>("behavior", true)},
                                                        {"searchReplace", {pair->first, pair->second}}});
                if (r.is_array())
                    for (const json& n : r)
                        out.push_back(n);
                else
                    out.push_back(r);
            }
            return out;
        },
    });
}

void registerActions(ext::Context& ctx)
{
    using ui::Action;
    const char* cat = "Puppeteer Joints";
    ui::actions::add(ctx, {.id = "puppeteer.joint.create", .label = "Create Joints", .category = cat,
                           .run = [] { ui::actions::run("rig.jointTool"); },
                           .doc = "Click in the viewport to place joints, each a child of the last."});
    ui::actions::add(ctx, {.id = "puppeteer.joint.insert", .label = "Insert Joint", .category = cat,
                           .doc = "Split the selected joint's bone with a new joint.", .command = "insertJoint"});
    ui::actions::add(ctx, {.id = "puppeteer.joint.parent", .label = "Parent Joints", .category = cat,
                           .doc = "Parent the selected joints under the last selected one.", .command = "parent"});
    ui::actions::add(ctx, {.id = "puppeteer.joint.unparent", .label = "Unparent Joints", .category = cat,
                           .doc = "Move the selected joints to the world, keeping where they are.", .command = "parent",
                           .call = {{"world", true}}});
    ui::actions::add(ctx, {.id = "puppeteer.joint.orient", .label = "Orient Joints", .category = cat,
                           .doc = "Aim the selected joints' primary axis down the bone, the secondary axis leaning toward a "
                                  "world axis.",
                           .command = "orientJoint"});
    ui::actions::add(ctx, {.id = "puppeteer.joint.center", .label = "Center Among Selected Vertices", .category = cat,
                           .doc = "Move the joint to the centre of the selected vertices, edges or faces.", .command = "snapToComponentCenter"});
    ui::actions::add(ctx, {.id = "puppeteer.joint.centerInMesh", .label = "Center in Mesh", .category = cat,
                           .doc = "Centre the selected joints in the cross-section of the meshes around them.", .command = "centerJoint"});
    ui::actions::add(ctx, {.id = "puppeteer.joint.mirror", .label = "Mirror Joints by Side", .category = cat,
                           .doc = "Mirror the selected joints to the other side, named by their side (arm_l to arm_r).",
                           .command = "puppeteerMirrorJoints"});
}
} // namespace

void registerJointTools(ext::Context& ctx)
{
    registerCommands(ctx);
    registerActions(ctx);
}
} // namespace puppeteer
