#include "ui/DominantInfluenceUi.h"

#include "core/PrefStore.h"
#include "scene/DominantInfluence.h"
#include "scene/Graph.h"
#include "scene/SelectAs.h"
#include "scene/Skin.h"
#include "ui/Actions.h"
#include "ui/SelectionHandlers.h"
#include "ui/ViewportTool.h"

#include <algorithm>
#include <cmath>

namespace lila::ui
{
namespace
{
constexpr const char* kHandlerId = "lila.dominantInfluence";
constexpr const char* kPref = "/rig/dominantInfluenceSelect";

scene::NodeId jointUnder(const selectionHandlers::Hit& h)
{
    if (h.node == scene::kNoNode || h.face == selectionHandlers::kNone)
        return scene::kNoNode;
    return scene::rig::dominantJoint(scene::Graph::current(), h.node, h.face);
}

/** The joint a click selects: the dominant one through its selectAs. */
scene::NodeId clickJoint(const selectionHandlers::Hit& h)
{
    scene::NodeId j = jointUnder(h);
    if (j == scene::kNoNode || selectionHandlers::selectAsBypassed())
        return j;
    return scene::rig::selectAsTarget(scene::Graph::current(), j);
}

std::optional<std::vector<std::string>> redirect(const selectionHandlers::Hit& h)
{
    scene::NodeId j = clickJoint(h);
    if (j == scene::kNoNode)
        return std::nullopt;
    return std::vector<std::string>{scene::Graph::current().node(j)->name()};
}

/** Shades everything a click there selects: the joint's own weights plus those of the helpers redirecting to it. */
std::optional<selectionHandlers::Highlight> hover(const selectionHandlers::Hit& h)
{
    scene::NodeId j = clickJoint(h);
    if (j == scene::kNoNode)
        return std::nullopt;
    scene::Graph& g = scene::Graph::current();
    selectionHandlers::Highlight out;
    out.mesh = h.node;
    out.domain = selectionHandlers::Highlight::Domain::Point;
    const bool bypass = selectionHandlers::selectAsBypassed();
    if (scene::NodeId skin = scene::skin::find(g, h.node); skin != scene::kNoNode)
        for (const scene::skin::Influence& inf : scene::skin::influences(g, skin))
        {
            if (inf.joint == scene::kNoNode || (inf.joint != j && (bypass || scene::rig::selectAsTarget(g, inf.joint) != j)))
                continue;
            std::vector<float> w = scene::rig::influenceWeights(g, h.node, inf.joint);
            if (out.weights.size() < w.size())
                out.weights.resize(w.size(), 0.0f);
            for (size_t p = 0; p < w.size(); ++p)
                out.weights[p] = std::min(1.0f, out.weights[p] + w[p]);
        }
    if (out.weights.empty())
        return std::nullopt;
    return out;
}

QString roleLabel(scene::rig::CandidateRole r)
{
    switch (r)
    {
    case scene::rig::CandidateRole::Redirect: return {};
    case scene::rig::CandidateRole::Weighted: return "weighted";
    case scene::rig::CandidateRole::Parent: return "parent";
    case scene::rig::CandidateRole::Child: return "child";
    case scene::rig::CandidateRole::Influence: return "influence";
    }
    return {};
}

const char* roleId(scene::rig::CandidateRole r)
{
    switch (r)
    {
    case scene::rig::CandidateRole::Redirect: return "redirect";
    case scene::rig::CandidateRole::Weighted: return "weighted";
    case scene::rig::CandidateRole::Parent: return "parent";
    case scene::rig::CandidateRole::Child: return "child";
    case scene::rig::CandidateRole::Influence: return "influence";
    }
    return "";
}

std::vector<selectionHandlers::Candidate> candidates(const selectionHandlers::Hit& h)
{
    std::vector<selectionHandlers::Candidate> out;
    if (h.node == scene::kNoNode || h.face == selectionHandlers::kNone)
        return out;
    scene::Graph& g = scene::Graph::current();
    for (const scene::rig::SelectCandidate& c :
         scene::rig::selectionCandidates(g, h.node, h.face, h.position, !selectionHandlers::selectAsBypassed()))
    {
        selectionHandlers::Candidate e;
        e.name = g.node(c.joint)->name();
        e.role = roleId(c.role);
        e.weight = c.weight;
        QString note = roleLabel(c.role);
        if (c.weight > 0.0f && c.role != scene::rig::CandidateRole::Parent && c.role != scene::rig::CandidateRole::Child)
            note = note.isEmpty() ? QString("%1%").arg(int(std::lround(c.weight * 100))) : QString("%1 %2%").arg(note).arg(int(std::lround(c.weight * 100)));
        e.label = note.isEmpty() ? QString::fromStdString(e.name) : QString("%1  (%2)").arg(QString::fromStdString(e.name), note);
        out.push_back(std::move(e));
    }
    return out;
}

/** Turns the handler on or off to match the pref, leaving any other active handler (a mode's) alone when off. */
void applyPref()
{
    bool on = PrefStore::get().value<bool>("general", kPref, false);
    if (on)
        selectionHandlers::setActive(kHandlerId);
    else if (selectionHandlers::activeId() == kHandlerId)
        selectionHandlers::setActive("");
}
} // namespace

void registerDominantInfluenceUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    selectionHandlers::add({.id = kHandlerId, .label = "Joint under mesh", .redirect = redirect, .hover = hover, .candidates = candidates});
    actions::add({.id = "rig.selectAsToggle",
                  .label = "Bypass Selection Redirects",
                  .category = "Rig",
                  .run =
                      [] {
                          bool on = PrefStore::get().value<bool>("general", "/rig/selectAsBypass", false);
                          PrefStore::get().set("general", "/rig/selectAsBypass", !on);
                          viewportTools::requestRedraw();
                      },
                  .doc = "Mesh clicks select the joint that weights the face itself (a twist bone) instead of its selectAs "
                         "target, until toggled back."});
    actions::add({.id = "rig.selectAsBypassOn",
                  .label = "Bypass Selection Redirects (Hold)",
                  .category = "Rig",
                  .run = [] { selectionHandlers::setSelectAsHeld(true); },
                  .doc = "Press of a held key: while it is down, mesh clicks ignore selectAs redirects."});
    actions::add({.id = "rig.selectAsBypassOff",
                  .label = "Bypass Selection Redirects (Release)",
                  .category = "Rig",
                  .run = [] { selectionHandlers::setSelectAsHeld(false); },
                  .doc = "Release of the held key bound to rig.selectAsBypassOn."});
    actions::add({.id = "rig.autoSelectAs",
                  .label = "Auto Selection Redirects",
                  .category = "Rig",
                  .doc = "Point the twist, corrective and muscle joints under the selection (every joint with none "
                         "selected) at their limb bone, so mesh clicks on them select the limb.",
                  .python = "autoSelectAs()",
                  .command = "autoSelectAs"});

    actions::add({.id = "rig.dominantInfluenceSelect",
                  .label = "Select Joints by Mesh",
                  .category = "Rig",
                  .run =
                      [] {
                          bool on = PrefStore::get().value<bool>("general", kPref, false);
                          PrefStore::get().set("general", kPref, !on);
                          applyPref();
                          viewportTools::requestRedraw();
                      },
                  .doc = "Clicking a skinned mesh selects the joint that most influences the face under the cursor; "
                         "hovering shades that joint's weights."});
    actions::add({.id = "rig.centerJoint",
                  .label = "Center Joint in Mesh",
                  .category = "Rig",
                  .doc = "Moves the selected joints to the centre of the mesh cross-section across their bones.",
                  .python = "centerJoint()",
                  .command = "centerJoint"});
    actions::add({.id = "rig.snapToComponentCenter",
                  .label = "Snap to Component Center",
                  .category = "Rig",
                  .doc = "Moves the selected transforms or joints to the centre of the selected vertices, edges or faces.",
                  .python = "snapToComponentCenter()",
                  .command = "snapToComponentCenter"});
    applyPref();
}
} // namespace lila::ui
