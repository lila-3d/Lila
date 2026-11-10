// Puppeteer's two modes. Rig mode builds characters (joints, IK chains, feet, lookers, spaces); Animate mode poses the
// joints directly: clicking the mesh picks the joint under it, the Spinner moves it, and a Space tap swaps the window to
// a full-screen graph editor while a Space hold is still the hotbox.

#include "Puppeteer.h"
#include "rig/Rig.h"

#include <lila/Events.h>
#include <lila/UI.h>
#include <lila/Viewport.h>

#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace
{
constexpr const char* kRigLayout = "Puppeteer Rig";
constexpr const char* kAnimateLayout = "Puppeteer Animate";
constexpr const char* kShelf = "Puppeteer";
constexpr const char* kGraphDock = "graphEditor";
constexpr const char* kTimeSliderDock = "timeSlider";
constexpr const char* kBaseSelection = "lila.dominantInfluence";

// Graph editor swap -------------------------------------------------------------------------------------------------

struct GraphSwap
{
    QPointer<QMainWindow> window;
    QByteArray state;
    bool on = false;
};

GraphSwap& graphSwap()
{
    static GraphSwap s;
    return s;
}

QMainWindow* mainWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* m = qobject_cast<QMainWindow*>(w); m && m->isVisible())
            return m;
    return nullptr;
}

/** The window becomes the graph editor (and time slider) alone; off puts back exactly what was there. */
void setGraphFullScreen(bool on)
{
    GraphSwap& s = graphSwap();
    if (on == s.on)
        return;
    if (!on)
    {
        s.on = false;
        if (QMainWindow* w = s.window.data())
        {
            if (QWidget* c = w->centralWidget())
                c->show();
            w->restoreState(s.state);
        }
        return;
    }
    QMainWindow* w = mainWindow();
    if (!w)
        return;
    s.window = w;
    s.state = w->saveState();
    s.on = true;
    panels::show(kGraphDock);
    for (QDockWidget* d : w->findChildren<QDockWidget*>())
        if (d->parentWidget() == w && !d->isFloating() && d->objectName() != kGraphDock && d->objectName() != kTimeSliderDock)
            d->hide();
    // With the viewports hidden the docks share the whole window.
    if (QWidget* c = w->centralWidget())
        c->hide();
    if (QDockWidget* g = w->findChild<QDockWidget*>(kGraphDock))
    {
        g->show();
        g->raise();
    }
}

// Selection -----------------------------------------------------------------------------------------------------------

std::optional<std::vector<std::string>> baseRedirect(const selectionHandlers::Hit& hit)
{
    const selectionHandlers::Handler* base = selectionHandlers::find(kBaseSelection);
    return base && base->redirect ? base->redirect(hit) : std::nullopt;
}

std::optional<selectionHandlers::Highlight> baseHover(const selectionHandlers::Hit& hit)
{
    const selectionHandlers::Handler* base = selectionHandlers::find(kBaseSelection);
    return base && base->hover ? base->hover(hit) : std::nullopt;
}

std::vector<selectionHandlers::Candidate> baseCandidates(const selectionHandlers::Hit& hit)
{
    const selectionHandlers::Handler* base = selectionHandlers::find(kBaseSelection);
    return base && base->candidates ? base->candidates(hit) : std::vector<selectionHandlers::Candidate>{};
}

/** An IK chain stands for its controller (its reverse foot or handle); a joint is always itself, posed FK. */
std::string animateTarget(Graph& g, const std::string& name)
{
    Node* n = named(g, name);
    if (n && n->type().isA(kIkChainType))
    {
        const NodeId goal = ikGoalOf(g, n->id());
        return goal ? g.node(goal)->path() : name;
    }
    return name;
}

/** The RMB joint menu picks as a click would: a chain stands for its controller. */
std::vector<selectionHandlers::Candidate> animateCandidates(const selectionHandlers::Hit& hit)
{
    std::vector<selectionHandlers::Candidate> out = baseCandidates(hit);
    for (selectionHandlers::Candidate& c : out)
        c.name = animateTarget(Graph::current(), c.name);
    return out;
}

std::optional<std::vector<std::string>> animateRedirect(const selectionHandlers::Hit& hit)
{
    Graph& g = Graph::current();
    std::vector<std::string> names;
    if (auto base = baseRedirect(hit))
        names = *base;
    else if (hit.node != kNoNode && g.node(hit.node) && (rig::isJoint(*g.node(hit.node)) || g.node(hit.node)->type().isA(kIkChainType)))
        names = {g.node(hit.node)->path()};
    else
        return std::nullopt;
    for (std::string& n : names)
        n = animateTarget(g, n);
    return names;
}

// Registration --------------------------------------------------------------------------------------------------------

json preset(std::vector<std::string> open, std::vector<std::string> raise, std::string menuSet, json size = json::object())
{
    return {{"version", 1},
            {"preset", {{"open", open}, {"raise", raise}, {"size", std::move(size)}}},
            {"menuSet", std::move(menuSet)},
            {"viewport", {{"quad", false}, {"camera", "persp"}}}};
}

void registerModeActions(ext::Context& ctx)
{
    const char* rigCat = "Puppeteer Rig";
    const char* animCat = "Puppeteer Animate";
    actions::add(ctx, {.id = "puppeteer.rig.character", .label = "Create Character", .category = rigCat,
                       .doc = "Make a Puppeteer character for the selected skeleton, finding its meshes, template and poses.",
                       .command = "puppeteerCharacter"});
    actions::add(ctx, {.id = "puppeteer.rig.ikChain", .label = "Create IK Chain", .category = rigCat,
                       .doc = "Select the start joint, then the end effector: an IK chain whose controller poses the joints' rotations.",
                       .command = "puppeteerIkChain"});
    actions::add(ctx, {.id = "puppeteer.rig.reverseFoot", .label = "Create Reverse Foot", .category = rigCat,
                       .doc = "Select an ankle with an IK chain: a sole control with heel, ball and toe roll, twist and bank.",
                       .command = "puppeteerReverseFoot"});
    actions::add(ctx, {.id = "puppeteer.rig.looker", .label = "Create Looker", .category = rigCat,
                       .doc = "Select a joint (a head, an eye): a target it aims at.", .command = "puppeteerLooker"});
    actions::add(ctx, {.id = "puppeteer.rig.space", .label = "Create Spaces", .category = rigCat,
                       .run =
                           [] {
                               Graph& g = Graph::current();
                               std::vector<std::string> sel;
                               for (NodeId id : g.selection())
                                   sel.push_back(g.node(id)->path());
                               if (sel.size() < 2)
                               {
                                   log::write(LogLevel::Warning, "Create Spaces: select the spaces, then the joint last");
                                   return;
                               }
                               json spaces = json::array();
                               for (size_t i = 0; i + 1 < sel.size(); ++i)
                                   spaces.push_back(sel[i]);
                               runCommand("puppeteerSpace", {{"args", {sel.back()}}, {"spaces", spaces}});
                           },
                       .doc = "Select the spaces, then the joint last: the joint can then be keyed in any of them."});

    actions::add(ctx, {.id = "puppeteer.anim.lock", .label = "Lock or Unlock Key", .category = animCat,
                       .doc = "Plant the selected controllers' key at this frame, or free it: a locked foot or hand holds until the next key.",
                       .command = "puppeteerFootLock"});
    actions::add(ctx, {.id = "puppeteer.anim.pin", .label = "Pin or Unpin Joints", .category = animCat,
                       .doc = "Pin the selected joints so full-body moves hold them where they are, or unpin them.",
                       .command = "puppeteerPin"});
    actions::add(ctx, {.id = "puppeteer.anim.matchControllers", .label = "Match Controllers to Pose", .category = animCat,
                       .doc = "Put the selected IK controllers on the joints' current pose.", .command = "ikMatch"});
    actions::add(ctx, {.id = "puppeteer.anim.snapFoot", .label = "Snap Foot to Sole", .category = animCat,
                       .doc = "Put the selected reverse feet on the sole of the pose, roll, twist and bank zeroed.",
                       .command = "puppeteerSnapReverseFoot"});
    actions::add(ctx, {.id = "puppeteer.anim.alignFoot", .label = "Align Foot Level", .category = animCat,
                       .doc = "Level the selected reverse feet, keeping the pose in roll, twist and bank.",
                       .command = "puppeteerSnapReverseFoot", .call = {{"align", true}}});
    actions::add(ctx, {.id = "puppeteer.graphFullScreen", .label = "Full-Screen Graph Editor", .category = animCat,
                       .run = [] { setGraphFullScreen(!graphSwap().on); },
                       .doc = "Swap the window to the graph editor alone, and back. Space tap in Animate mode."});
}

void registerMenus(ext::Context& ctx)
{
    menus::addMenu(ctx, {.id = "Puppeteer", .title = "Puppeteer", .sets = {"Rig", "Animate"}});
    for (const char* a : {"mode.puppeteerRig", "mode.puppeteerAnimate", "", "puppeteer.rig.character", "window.puppeteerStringsRules", "puppeteer.rig.ikChain",
                          "puppeteer.rig.reverseFoot", "puppeteer.rig.looker", "puppeteer.rig.space", "rig.bendBones",
                          "rig.bendBoneCV", "rig.mirrorBoneCVs", "rig.bakeBendBones"})
        menus::addItem(ctx, *a ? menus::Item{.menu = "Puppeteer", .action = a} : menus::Item{.menu = "Puppeteer", .kind = menus::Kind::Separator});
    menus::addItem(ctx, {.menu = "Puppeteer", .kind = menus::Kind::Submenu, .label = "Joints"});
    for (const char* a : {"puppeteer.joint.create", "puppeteer.joint.insert", "puppeteer.joint.parent", "puppeteer.joint.unparent",
                          "puppeteer.joint.orient", "puppeteer.joint.center", "puppeteer.joint.centerInMesh", "puppeteer.joint.mirror"})
        menus::addItem(ctx, {.menu = "Puppeteer/Joints", .action = a});
    menus::addItem(ctx, {.menu = "Puppeteer", .kind = menus::Kind::Separator});
    for (const char* a : {"puppeteer.anim.lock", "puppeteer.anim.pin", "puppeteer.anim.matchControllers", "puppeteer.anim.snapFoot",
                          "puppeteer.anim.alignFoot", "puppeteer.spinner.inheritRotate", "puppeteer.spinner.toonScale",
                          "puppeteer.spinner.autoPose", "puppeteer.graphFullScreen"})
        menus::addItem(ctx, {.menu = "Puppeteer", .action = a});
    menus::addItem(ctx, {.menu = "Puppeteer", .kind = menus::Kind::Submenu, .label = "Strings"});
    for (const char* a : {"puppeteer.strings.forward", "puppeteer.strings.backward", "puppeteer.strings.ik", "puppeteer.strings.fullBody",
                          "", "puppeteer.strings.lineOfAction", "window.puppeteerStringsRules", "", "puppeteer.strings.contacts", "puppeteer.strings.onLayer", "puppeteer.strings.stretch", "puppeteer.strings.zero", "puppeteer.strings.restoreLengths",
                          "puppeteer.strings.overshoot", "puppeteer.strings.smooth", "puppeteer.strings.fill", "puppeteer.strings.retime", "puppeteer.strings.trail", "",
                          "puppeteer.strings.breakdownParts", "puppeteer.strings.clearBreakdownParts", "",
                          "puppeteer.strings.glue", "puppeteer.strings.attach", "puppeteer.strings.unglue"})
        menus::addItem(ctx, *a ? menus::Item{.menu = "Puppeteer/Strings", .action = a}
                               : menus::Item{.menu = "Puppeteer/Strings", .kind = menus::Kind::Separator});
    for (const char* a : {"puppeteer.strings.smooth", "puppeteer.strings.fill", "puppeteer.strings.retime"})
        menus::addItem(ctx, {.menu = menus::kTimeSliderRange, .action = a});
}

void registerShelf(ext::Context& ctx)
{
    json items = json::array();
    for (auto [label, iconName, target] : {std::tuple{"Rig", "bone", "mode.puppeteerRig"},
                                           {"Animate", "walk", "mode.puppeteerAnimate"},
                                           {"Character", "user-plus", "puppeteer.rig.character"},
                                           {"Joints", "hierarchy-2", "puppeteer.joint.create"},
                                           {"Orient", "compass", "puppeteer.joint.orient"},
                                           {"Mirror", "flip-horizontal", "puppeteer.joint.mirror"},
                                           {"IK Chain", "vector", "puppeteer.rig.ikChain"},
                                           {"Foot", "shoe", "puppeteer.rig.reverseFoot"},
                                           {"Looker", "eye", "puppeteer.rig.looker"},
                                           {"Spaces", "world", "puppeteer.rig.space"},
                                           {"Lock", "lock", "puppeteer.anim.lock"},
                                           {"Pin", "pin", "puppeteer.anim.pin"},
                                           {"Match", "target", "puppeteer.anim.matchControllers"},
                                           {"Stacker", "list-numbers", "window.puppeteerStacker"},
                                           {"Mixer", "stack-2", "window.puppeteerMixer"},
                                           {"Bank", "movie", "window.puppeteerAnimBank"},
                                           {"Cycles", "repeat", "window.puppeteerCycleMaker"}})
        items.push_back({{"label", label}, {"icon", iconName}, {"target", target}});
    shelves::add(ctx, {kShelf, std::move(items)});
}

void registerModeBundles(ext::Context& ctx)
{
    layouts::add(ctx, kRigLayout,
                 preset({"outliner", "channelBox", "attributeEditor", "displayLayers", "timeSlider"}, {"channelBox"}, "Rig"));
    layouts::add(ctx, kAnimateLayout,
                 preset({"outliner", "channelBox", "attributeEditor", "displayLayers", "graphEditor", "timeSlider"}, {"graphEditor", "channelBox"}, "Animate",
                        {{"graphEditor", {{"height", 0.3}}}}));

    selectionHandlers::add(ctx, {.id = kRigSelection, .label = "Puppeteer Rig: joint under mesh", .redirect = baseRedirect, .hover = baseHover,
                                 .candidates = baseCandidates});
    selectionHandlers::add(ctx, {.id = kAnimateSelection, .label = "Puppeteer Animate: joint under mesh, IK goals for IK chains",
                                 .redirect = animateRedirect, .hover = baseHover, .candidates = animateCandidates});

    const json common = {{"I", {{"press", "puppeteer.spinner.move"}}},
                         {"O", {{"press", "puppeteer.spinner.rotate"}}},
                         {"P", {{"press", "puppeteer.spinner.scale"}}},
                         {"Shift+O", {{"press", "puppeteer.spinner.inheritRotate"}}},
                         {"8", {{"press", "puppeteer.spinner.local"}}},
                         {"9", {{"press", "puppeteer.spinner.world"}}},
                         {"0", {{"press", "puppeteer.spinner.screen"}}},
                         {"`", {{"press", "puppeteer.spinner.summonDown"}, {"release", "puppeteer.spinner.summonUp"}}}};
    json rigKeys = {{"viewport", common}};
    rigKeys["viewport"]["J"] = {{"press", "puppeteer.joint.create"}};
    rigKeys["viewport"]["Shift+J"] = {{"press", "puppeteer.joint.orient"}};
    rigKeys["viewport"]["Alt+M"] = {{"press", "puppeteer.joint.mirror"}};
    json animKeys = {{"viewport", common}, {"graphEditor", {{"Space", {{"press", "puppeteer.graphFullScreen"}}}}}};
    animKeys["viewport"]["Space"] = {{"press", "puppeteer.graphFullScreen"}};
    animKeys["viewport"]["Shift+L"] = {{"press", "puppeteer.anim.lock"}};
    animKeys["viewport"]["Shift+P"] = {{"press", "puppeteer.anim.pin"}};
    animKeys["viewport"]["U"] = {{"LMB", {{"markingMenu", "puppeteerStrings"}}}};

    modes::add(ctx, {.id = kRigMode,
                     .label = "Puppeteer Rig",
                     .doc = "Build characters: joints, IK chains, reverse feet, lookers and spaces.",
                     .icon = "bone",
                     .layout = kRigLayout,
                     .hotkeys = rigKeys,
                     .shelf = kShelf,
                     .gizmoProvider = kSpinnerGizmo,
                     .selectionHandler = kRigSelection});
    modes::add(ctx, {.id = kAnimateMode,
                     .label = "Puppeteer Animate",
                     .doc = "Pose the joints directly: click the mesh to pick the joint under it, turn it with the Spinner. "
                            "U + left drag: Strings modes. Space tap: full-screen graph editor; Space hold: the hotbox.",
                     .icon = "walk",
                     .layout = kAnimateLayout,
                     .hotkeys = animKeys,
                     .shelf = kShelf,
                     .gizmoProvider = kSpinnerGizmo,
                     .selectionHandler = kAnimateSelection,
                     // Animating starts in Rotate, whatever tool was on before.
                     .onEnter =
                         [] {
                             if (actions::find("tool.rotate"))
                                 actions::run("tool.rotate");
                         },
                     .onExit = [] { setGraphFullScreen(false); }});
}
} // namespace

void registerModes(ext::Context& ctx)
{
    registerModeActions(ctx);
    registerMenus(ctx);
    registerModeBundles(ctx);
    registerShelf(ctx);
    // Unloading while swapped must give the window its viewports back.
    ctx.track("puppeteer:graphSwap", [] { setGraphFullScreen(false); });
}
} // namespace puppeteer
