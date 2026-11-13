// Props and weapons: prop and grip point nodes, fitting points to a mesh, holding a prop in one or both hands, keyed
// handoffs between hands, holsters and the world, aiming, the solve that shares the load between the arms (a grapple
// with frame contacts), and setting a prop up for Unreal.

#include "prop/Props.h"

#include "Puppeteer.h"
#include "hand/Hands.h"
#include "rig/Rig.h"

#include <lila/Anim.h>
#include <lila/AnimCurve.h>
#include <lila/Grapple.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/OptionBox.h>
#include <lila/Pins.h>
#include <lila/Relocate.h>
#include <lila/Simulation.h>
#include <lila/Spaces.h>

#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace puppeteer::prop
{
namespace gp = lila::scene::grapple;
namespace sp = lila::scene::spaces;

namespace
{
const std::vector<std::string>& classIds()
{
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v;
        for (const Preset& p : presets())
            v.push_back(p.id);
        return v;
    }();
    return ids;
}
const std::vector<std::string>& roleIds()
{
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v;
        for (int i = 0; i < int(Role::Count); ++i)
            v.push_back(roleId(Role(i)));
        return v;
    }();
    return ids;
}

int64_t intOf(Graph& g, NodeId id, const char* attr) { return std::get<int64_t>(g.value(plugOf(*g.node(id), attr))); }
bool boolOf(Graph& g, NodeId id, const char* attr) { return std::get<bool>(g.value(plugOf(*g.node(id), attr))); }
double numOf(Graph& g, NodeId id, const char* attr) { return std::get<double>(g.value(plugOf(*g.node(id), attr))); }
std::string strOf(Graph& g, NodeId id, const char* attr) { return std::get<std::string>(g.value(plugOf(*g.node(id), attr))); }
void setPlug(Graph& g, NodeId id, const char* attr, PlugValue v) { g.setValue(plugOf(*g.node(id), attr), std::move(v)); }

glm::dvec3 up() { return lila::math::axisBasis().up; }

double fps() { return std::max(anim::playback().fps, 1e-6); }

/** Sets the scene's time and evaluates. */
void at(Graph& g, double t)
{
    g.setTime(t, false);
    g.evaluate();
}

/** Puts the scene's time back when it leaves scope. */
struct KeepTime
{
    Graph& g;
    double was;
    explicit KeepTime(Graph& graph) : g(graph), was(graph.time()) {}
    ~KeepTime()
    {
        g.setTime(was, false);
        g.evaluate();
    }
};
} // namespace

// Reading props ------------------------------------------------------------------------------------------------------

NodeId Prop::point(Graph& g, Role r) const
{
    for (NodeId p : points)
        if (roleOf(*g.node(p)) == r)
            return p;
    return kNoNode;
}

Role roleOf(const Node& point) { return Role(std::clamp<int64_t>(point.as<int64_t>(attrOf(point, "role")), 0, int64_t(Role::Count) - 1)); }

NodeId propFor(Graph& g, NodeId node)
{
    for (NodeId cur = node; cur; cur = g.parent(cur))
    {
        Node* n = g.node(cur);
        if (!n)
            break;
        if (n->type().isA(kPropType))
            return cur;
        for (NodeId c : g.children(cur))
            if (Node* cn = g.node(c); cn && cn->type().isA(kPropType))
                return c;
    }
    return kNoNode;
}

std::optional<Prop> load(Graph& g, NodeId propNode)
{
    Node* n = g.node(propNode);
    if (!n || !n->type().isA(kPropType) || !g.parent(propNode))
        return std::nullopt;
    Prop p;
    p.node = propNode;
    p.root = g.parent(propNode);
    p.cls = Class(std::clamp<int64_t>(intOf(g, propNode, "propClass"), 0, int64_t(Class::Count) - 1));
    p.rightHanded = intOf(g, propNode, "hand") == 0;
    p.twoHanded = boolOf(g, propNode, "twoHanded");
    for (NodeId c : g.children(propNode))
        if (Node* cn = g.node(c); cn && cn->type().isA(kPointType))
            p.points.push_back(c);
    return p;
}

std::vector<NodeId> props(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(kPropType))
            out.push_back(id);
    return out;
}

glm::dmat4 pointLocal(Graph& g, const Prop& p, NodeId point) { return glm::inverse(rigidOf(worldOf(g, p.root))) * rigidOf(worldOf(g, point)); }

std::vector<NodeId> propMeshes(Graph& g, const Prop& p)
{
    std::vector<NodeId> out;
    std::vector<NodeId> all{p.root};
    for (NodeId d : g.descendants(p.root))
        all.push_back(d);
    for (NodeId id : all)
        if (Node* n = g.node(id); n && n->type().attrIndex("outMesh") >= 0 && !n->type().isA(kPointType))
            out.push_back(id);
    return out;
}

namespace
{
std::string spaceName(Graph& g, NodeId node, int index)
{
    for (const sp::Space& s : sp::list(g, node))
        if (s.index == index)
            return s.kind == sp::Kind::World ? "world" : s.kind == sp::Kind::Parent ? "parent" : s.target ? g.path(s.target) : s.name;
    return "parent";
}

Hold holdFor(Graph& g, NodeId node, int index, double time)
{
    Hold h;
    h.time = time;
    h.space = spaceName(g, node, index);
    h.kind = h.space == "world" ? "world" : h.space == "parent" ? "parent" : "node";
    for (const sp::Space& s : sp::list(g, node))
        if (s.index == index && s.kind == sp::Kind::Node)
        {
            h.target = s.target;
            if (Node* t = g.node(s.target))
            {
                if (t->type().isA(kPointType))
                    h.kind = "holster";
                else if (const NodeId hn = hand::handFor(g, s.target); hn && hand::load(g, hn) && hand::load(g, hn)->joint == s.target)
                {
                    h.kind = "hand";
                    h.side = hand::load(g, hn)->side == hand::Side::Left ? "left" : "right";
                }
            }
        }
    return h;
}
} // namespace

std::vector<Hold> holdsOf(Graph& g, const Prop& p)
{
    std::vector<Hold> out;
    const NodeId curve = anim::curveFor(g, {p.root, Transform::kSpace, -1});
    if (!curve)
    {
        out.push_back(holdFor(g, p.root, sp::active(g, p.root), anim::playback().minTime));
        return out;
    }
    for (double t : anim::curveKeyTimes(*g.node(curve)))
    {
        const int index = int(std::llround(anim::curveValueAt(*g.node(curve), 0, t)));
        if (!out.empty() && spaceName(g, p.root, index) == out.back().space)
            continue;
        out.push_back(holdFor(g, p.root, index, t));
    }
    return out;
}

NodeId holderNow(Graph& g, const Prop& p)
{
    const int index = sp::active(g, p.root);
    for (const sp::Space& s : sp::list(g, p.root))
        if (s.index == index && s.kind == sp::Kind::Node && s.target && rig::isJoint(*g.node(s.target)))
            return s.target;
    return kNoNode;
}

NodeId propGrapple(Graph& g, NodeId character, bool make)
{
    const std::string ch = g.path(character);
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA(gp::kType) && attrOf(*n, kGrappleCharacterAttr) >= 0 && textOf(*n, kGrappleCharacterAttr) == ch)
            return id;
    if (!make)
        return kNoNode;
    Node* gn = g.createNode(*types::find(gp::kType), g.node(character)->name() + "_props");
    const NodeId id = gn->id();
    g.addAttr(id, {.name = kGrappleCharacterAttr,
                   .type = PlugType::String,
                   .defaultValue = std::string(),
                   .flags = AttrInput | AttrStorable | AttrHidden,
                   .doc = "The character whose props this grapple solves."});
    setPlug(g, id, kGrappleCharacterAttr, ch);
    // One character: no bodies to keep apart, and the arms alone carry the grips.
    setPlug(g, id, "collide", false);
    setPlug(g, id, "autoPose", false);
    setPlug(g, id, "iterations", int64_t(8));
    g.evaluate();
    gp::setFigures(g, id, {gp::fitFigure(g, characterRoot(g, character))});
    return id;
}

json propJson(Graph& g, const Prop& p)
{
    const Preset& pr = preset(p.cls);
    json points = json::array();
    for (NodeId pt : p.points)
    {
        const Node& n = *g.node(pt);
        points.push_back({{"point", g.path(pt)},
                          {"name", n.name()},
                          {"role", roleId(roleOf(n))},
                          {"radius", numOf(g, pt, "radius")},
                          {"position", vecJson(glm::dvec3(worldOf(g, pt)[3]))}});
    }
    json holds = json::array();
    for (const Hold& h : holdsOf(g, p))
    {
        json j = {{"time", h.time}, {"frame", h.time * fps()}, {"space", h.space}, {"kind", h.kind}};
        if (!h.side.empty())
            j["side"] = h.side;
        holds.push_back(j);
    }
    const NodeId holder = holderNow(g, p);
    return {{"prop", g.path(p.node)},
            {"root", g.path(p.root)},
            {"class", pr.id},
            {"label", pr.label},
            {"hand", p.rightHanded ? "right" : "left"},
            {"twoHanded", p.twoHanded},
            {"holster", strOf(g, p.node, "holster")},
            {"points", points},
            {"holds", holds},
            {"holder", holder ? json(g.path(holder)) : json(nullptr)}};
}

// Commands ------------------------------------------------------------------------------------------------------------

namespace
{
std::vector<glm::dvec3> meshPointsIn(Graph& g, const Prop& p)
{
    std::vector<glm::dvec3> out;
    const glm::dmat4 toRoot = glm::inverse(worldOf(g, p.root));
    for (NodeId m : propMeshes(g, p))
    {
        const MeshPtr mesh = std::get<MeshPtr>(g.value({m, g.node(m)->type().attrIndex("outMesh"), -1}));
        if (!mesh)
            continue;
        const glm::dmat4 xf = toRoot * worldOf(g, m);
        for (const glm::dvec3& q : mesh->positions())
            out.push_back(glm::dvec3(xf * glm::dvec4(q, 1.0)));
    }
    return out;
}

/** A point's node name: after its prop, so points of several props read apart in the outliner and never take a mesh's name. */
std::string pointName(Graph& g, const Prop& p, const std::string& name) { return g.node(p.root)->name() + "_" + name + "_point"; }

/** Makes or moves a point under the prop node to frame (prop root space). */
NodeId writePoint(Graph& g, const Prop& p, const PointDef& d, NodeId existing = kNoNode)
{
    NodeId id = existing;
    if (!id)
    {
        Node* n = g.createNode(*types::find(kPointType), pointName(g, p, d.name), p.node);
        id = n->id();
    }
    setPlug(g, id, "role", int64_t(d.role));
    setPlug(g, id, "radius", d.radius);
    g.evaluate();
    // The prop node rides the root with no offset, so the point's world is the root's times its frame.
    const glm::dmat4 world = rigidOf(worldOf(g, p.root)) * d.frame;
    setWorldRotation(g, id, glm::dmat3(world));
    g.evaluate();
    setWorldPosition(g, id, glm::dvec3(world[3]));
    g.evaluate();
    return id;
}

json fitProp(Graph& g, Prop& p)
{
    g.evaluate();
    const glm::dvec3 upLocal = glm::normalize(glm::transpose(glm::dmat3(rigidOf(worldOf(g, p.root)))) * up());
    const Fit fit = fitPoints(p.cls, meshPointsIn(g, p), upLocal);
    // Refitting replaces the points the fit makes and keeps any the user added under other names.
    std::map<std::string, NodeId> byName;
    for (NodeId pt : p.points)
        byName[g.node(pt)->name()] = pt;
    for (const PointDef& d : fit.points)
    {
        const std::string name = pointName(g, p, d.name);
        const auto it = byName.find(name);
        writePoint(g, p, d, it == byName.end() ? kNoNode : it->second);
    }
    p = *load(g, p.node);
    if (fit.from != "mesh")
        log::write(LogLevel::Warning, "puppeteerPropFit: " + g.path(p.root) + ": " + fit.from);
    return {{"from", fit.from}, {"length", fit.length}, {"points", fit.points.size()}};
}

/** The prop named by a command's objects, else the selection, else the scene's only prop. */
Prop propArg(Graph& g, const Args& a, const char* cmd)
{
    std::vector<NodeId> cands;
    for (const std::string& s : objectArgs(a))
        cands.push_back(nodeNamed(g, s, cmd).id());
    if (cands.empty())
        cands = g.selection();
    for (NodeId c : cands)
        if (const NodeId pn = propFor(g, c))
            return *load(g, pn);
    const std::vector<NodeId> all = props(g);
    if (all.size() == 1 && objectArgs(a).empty())
        return *load(g, all.front());
    throw CommandError(std::string(cmd) + ": give a prop (puppeteerPropSetup makes one), or select it");
}

/** The character a prop command works with: the flag, the one holding the prop, the selection's, or the only one. */
NodeId characterArg(Graph& g, const Args& a, const Prop& p, const char* cmd)
{
    if (a.has("character"))
    {
        const NodeId c = characterFor(g, nodeNamed(g, a.get<std::string>("character"), cmd).id());
        if (!c)
            throw CommandError(std::string(cmd) + ": " + a.get<std::string>("character") + " is not in a Puppeteer character");
        return c;
    }
    for (const Hold& h : holdsOf(g, p))
        if (h.target)
            if (const NodeId c = characterFor(g, h.target))
                return c;
    for (NodeId s : g.selection())
        if (const NodeId c = characterFor(g, s))
            return c;
    const std::vector<NodeId> all = characters(g);
    if (all.size() == 1)
        return all.front();
    throw CommandError(std::string(cmd) + ": give character (the scene has " + std::to_string(all.size()) + ")");
}

struct HandRef
{
    hand::Hand h;
    bool right = true;
};

std::optional<HandRef> handOf(Graph& g, NodeId character, bool right)
{
    if (hand::handsOf(g, character).empty())
        setupHands(g, character);
    for (NodeId hn : hand::handsOf(g, character))
        if (std::optional<hand::Hand> h = hand::load(g, hn); h && (h->side == hand::Side::Right) == right && h->side != hand::Side::Center)
            return HandRef{*h, right};
    return std::nullopt;
}

HandRef needHand(Graph& g, NodeId character, bool right, const char* cmd)
{
    std::optional<HandRef> h = handOf(g, character, right);
    if (!h)
        throw CommandError(std::format("{}: {} has no {} hand the hand rig could find (puppeteerHandSetup)", cmd, g.node(character)->name(),
                                       right ? "right" : "left"));
    return *h;
}

/** The hand's measurements in its joint's rigid space. */
HandShape shapeOf(Graph& g, const hand::Hand& h)
{
    HandShape s;
    s.forward = h.forward;
    s.thumbSide = h.thumbSide;
    s.palm = h.palm;
    const glm::dmat4 toHand = glm::inverse(rigidOf(worldOf(g, h.joint)));
    glm::dvec3 k(0.0);
    int n = 0;
    for (const hand::Finger& f : h.fingers)
        if (f.role != hand::Role::Thumb && !f.joints.empty())
        {
            k += glm::dvec3(toHand * worldOf(g, f.joints.front())[3]);
            ++n;
        }
    if (n)
        s.knuckles = k / double(n);
    return s;
}

glm::dmat4 gripOf(Graph& g, const hand::Hand& h, NodeId point) { return gripFrame(shapeOf(g, h), numOf(g, point, "radius")); }

/** The grip point a hold uses: by name (or role id), else the role's first point. */
NodeId gripPoint(Graph& g, const Prop& p, const std::string& name, Role fallback, const char* cmd)
{
    if (!name.empty())
    {
        for (NodeId pt : p.points)
        {
            const std::string& n = g.node(pt)->name();
            if (n == name || n == pointName(g, p, name) || roleId(roleOf(*g.node(pt))) == name)
                return pt;
        }
        throw CommandError(std::format("{}: {} has no point named {}", cmd, g.node(p.root)->name(), name));
    }
    return p.point(g, fallback);
}

int ensureSpace(Graph& g, NodeId node, NodeId target)
{
    const int found = sp::find(g, node, g.path(target));
    if (found >= 0)
        return found;
    return sp::addSpace(g, node, g.path(target), false);
}

int worldSpace(Graph& g, NodeId node)
{
    const int found = sp::find(g, node, "world");
    return found >= 0 ? found : sp::addSpace(g, node, "world", false);
}

/** Keys the root's translate and rotate at t so its world matrix is world there (in its active space then). */
void keyRootAt(Graph& g, NodeId root, const glm::dmat4& world, double t)
{
    at(g, t);
    setWorldRotation(g, root, glm::dmat3(rigidOf(world)));
    g.evaluate();
    setWorldPosition(g, root, glm::dvec3(world[3]));
    const std::string path = g.path(root);
    const glm::dvec3 tr = vecOf(g, root, Transform::kTranslate), ro = vecOf(g, root, Transform::kRotate);
    keyVec(path, "translate", tr, t);
    keyVec(path, "rotate", ro, t);
    g.evaluate();
}

/**
 * Keys the prop settling from where it is at t into `end` (the point's frame in the space's frame) by t + blend: every
 * frame between, the point's frame against the space eases there rigidly about the point itself. Interpolating the
 * root's own channels instead swings a prop whose origin is far from its mesh through a wide arc.
 */
void settleInto(Graph& g, const Prop& p, NodeId point, NodeId spaceTarget, const glm::dmat4& end, double t, double blend)
{
    const glm::dmat4 local = pointLocal(g, p, point);
    auto spaceAt = [&](double time) {
        at(g, time);
        return rigidOf(worldOf(g, spaceTarget));
    };
    const glm::dmat4 start = glm::inverse(spaceAt(t)) * rigidOf(worldOf(g, point));
    const glm::dquat q0 = glm::quat_cast(glm::dmat3(start)), q1 = glm::quat_cast(glm::dmat3(end));
    const long long frames = std::max<long long>(1, std::llround(blend * fps()));
    for (long long f = 1; f <= frames; ++f)
    {
        const double time = blend <= 0.0 ? t : t + blend * double(f) / double(frames);
        const double u = double(f) / double(frames), e = u * u * (3.0 - 2.0 * u);
        glm::dmat4 rel = glm::dmat4(glm::mat3_cast(glm::slerp(q0, q1, e)));
        rel[3] = glm::dvec4(glm::mix(glm::dvec3(start[3]), glm::dvec3(end[3]), e), 1.0);
        keyRootAt(g, p.root, placeOn(spaceAt(time) * rel, local), time);
    }
}

/** Drops the bake layers of the character's prop grapple, leaving the animation itself. */
void dropSolve(Graph& g, NodeId grapple)
{
    if (!grapple)
        return;
    Node& n = *g.node(grapple);
    const json layers = json::parse(textOf(n, sim::kRecordingsAttr).empty() ? std::string("[]") : textOf(n, sim::kRecordingsAttr), nullptr, false);
    if (layers.is_array())
        for (const json& l : layers)
            if (l.is_string() && named(g, l.get<std::string>()))
                ops::execute("deleteAnimLayer", {{"args", {l.get<std::string>()}}});
    setPlug(g, grapple, sim::kRecordingsAttr, std::string("[]"));
    g.evaluate();
}

double weightAt(Graph& g, NodeId node, double t)
{
    at(g, t);
    return numOf(g, node, "weight");
}

/** Keys a weight from what it is at t to `to` at t + blend (a hard switch a frame apart with no blend). */
void rampWeight(Graph& g, NodeId node, double t, double blend, double to)
{
    const PlugRef plug = plugOf(*g.node(node), "weight");
    const double end = t + std::max(blend, 1.0 / fps());
    const double from = weightAt(g, node, t);
    const double later = weightAt(g, node, end);
    if (std::abs(from - to) < 1e-9 && std::abs(later - to) < 1e-9)
        return;
    anim::setKey(g, plug, t, from);
    anim::setKey(g, plug, end, to);
}

/** The prop's frame contacts in the character's grapple: source joint path, frame path, node. */
struct PropContact
{
    NodeId node;
    std::string source, target, frame;
};
std::vector<PropContact> propContacts(Graph& g, NodeId grapple, const Prop& p)
{
    std::vector<PropContact> out;
    if (!grapple)
        return out;
    std::set<std::string> mine;
    for (NodeId pt : p.points)
        mine.insert(g.path(pt));
    for (NodeId c : gp::contactsOf(g, grapple))
        if (gp::contactFollowsFrame(g, c) && mine.count(strOf(g, c, "frame")))
            out.push_back({c, strOf(g, c, "source"), strOf(g, c, "target"), strOf(g, c, "frame")});
    return out;
}

NodeId contactFor(Graph& g, NodeId grapple, const Prop& p, const hand::Hand& source, NodeId targetJoint, NodeId point)
{
    const std::string s = g.path(source.joint), t = targetJoint ? g.path(targetJoint) : std::string(), f = g.path(point);
    for (const PropContact& c : propContacts(g, grapple, p))
        if (c.source == s && c.target == t && c.frame == f)
            return c.node;
    Node* n = g.createNode(*types::find(gp::kContactType), g.node(source.joint)->name() + "_on_" + g.node(point)->name(), grapple);
    const NodeId id = n->id();
    setPlug(g, id, "source", s);
    setPlug(g, id, "target", t);
    setPlug(g, id, "frame", f);
    setPlug(g, id, "orient", true);
    setPlug(g, id, "gripOffset", gripOf(g, source, point));
    // Off until keyed on.
    setPlug(g, id, "weight", 0.0);
    anim::setKey(g, plugOf(*n, "weight"), anim::playback().minTime, 0.0);
    return id;
}

/** The hands holding the prop at t: by its space, plus every contact of it on there. */
std::set<NodeId> handsHolding(Graph& g, NodeId grapple, const Prop& p, double t)
{
    std::set<NodeId> out;
    at(g, t);
    if (const NodeId j = holderNow(g, p))
        out.insert(j);
    for (const PropContact& c : propContacts(g, grapple, p))
        if (numOf(g, c.node, "weight") > 0.5)
            if (Node* s = named(g, c.source))
                out.insert(s->id());
    return out;
}

/**
 * Closes the hand's fingers onto the prop from open, at the current time, keyed. A finger that starts in the prop opens
 * out of it and closes back onto it; only one that can't get out (its knuckle in the prop) keeps the class pose, which
 * puppeteerHandGrip logs.
 */
void wrapFingers(Graph& g, const hand::Hand& h, const Prop& p, const std::string& pose, bool trigger)
{
    json targets = json::array();
    for (NodeId m : propMeshes(g, p))
        targets.push_back(g.path(m));
    if (targets.empty())
        return;
    const std::string hn = g.path(h.node);
    const json fingers = trigger ? json::array({"thumb", "middle", "ring", "pinky"}) : json::array({"thumb", "index", "middle", "ring", "pinky"});
    ops::execute("puppeteerHandPose", {{"args", {hn}}, {"pose", "flat"}, {"fingers", fingers}, {"key", false}});
    const json got = ops::execute("puppeteerHandGrip", {{"args", {hn}}, {"target", targets}, {"fingers", fingers}, {"key", true}});
    json stuck = json::array();
    const json fingerResults = got.is_array() && !got.empty() ? got.at(0).value("fingers", json::object()) : json::object();
    for (const auto& [role, r] : fingerResults.items())
        if (r.value("stayed", false))
            stuck.push_back(role);
    if (!stuck.empty())
        ops::execute("puppeteerHandPose", {{"args", {hn}}, {"pose", pose}, {"fingers", stuck}, {"key", true}});
}

/** Keys a hand's fingers at t0 as they are, then poses (and wraps onto the prop's meshes) at t1. */
void poseFingers(Graph& g, const hand::Hand& h, double t0, double t1, const std::string& pose, bool wrap, bool trigger, const Prop* p)
{
    const std::string hn = g.path(h.node);
    at(g, t0);
    ops::execute("puppeteerHandKey", {{"args", {hn}}});
    at(g, t1);
    ops::execute("puppeteerHandPose", {{"args", {hn}}, {"pose", pose}, {"key", true}});
    if (wrap && p)
        wrapFingers(g, h, *p, pose, trigger);
}

double blendArg(const Args& a) { return std::max(0.0, a.get<double>("blend", 0.2)); }

/** Solves the character's prop grapple over the playback range onto its layer. */
json solveGrapple(Graph& g, NodeId grapple, const Args& a)
{
    json call = {{"args", {g.path(grapple)}}};
    if (a.has("start"))
        call["start"] = a.get<int64_t>("start");
    if (a.has("end"))
        call["end"] = a.get<int64_t>("end");
    return ops::execute("bakeGrapple", call);
}

const char* classFlagDoc()
{
    static const std::string doc = [] {
        std::string s = "The kind of prop:";
        for (const Preset& p : presets())
            s += std::string(" ") + p.id;
        return s + ".";
    }();
    return doc.c_str();
}

Class classArg(const Args& a, const char* cmd, Class fallback)
{
    if (!a.has("kind"))
        return fallback;
    const std::string id = a.get<std::string>("kind");
    if (const std::optional<Class> c = classFromId(id))
        return *c;
    throw CommandError(std::format("{}: no class '{}'; one of {}", cmd, id, [] {
        std::string s;
        for (const Preset& p : presets())
            s += (s.empty() ? "" : ", ") + std::string(p.id);
        return s;
    }()));
}

/** A joint of the character by bare name (no namespace), first in the given order; kNoNode when none matches. */
NodeId jointByNames(Graph& g, NodeId character, const std::vector<std::string>& names)
{
    const NodeId root = characterRoot(g, character);
    std::vector<NodeId> joints{root};
    for (NodeId d : g.descendants(root))
        if (rig::isJoint(*g.node(d)))
            joints.push_back(d);
    auto bare = [&](NodeId j) {
        std::string n = g.node(j)->name();
        if (const size_t c = n.rfind(':'); c != std::string::npos)
            n = n.substr(c + 1);
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        return n;
    };
    for (const std::string& want : names)
        for (NodeId j : joints)
            if (bare(j) == want)
                return j;
    return kNoNode;
}

/** The character's right, up and forward in the world, from its hands (right minus left, levelled). */
glm::dmat3 characterFrame(Graph& g, NodeId character)
{
    const glm::dvec3 u = up();
    glm::dvec3 right(-1.0, 0.0, 0.0);
    const std::optional<HandRef> r = handOf(g, character, true), l = handOf(g, character, false);
    if (r && l)
    {
        NodeId ra = g.parent(g.parent(r->h.joint)), la = g.parent(g.parent(l->h.joint));
        glm::dvec3 d = glm::dvec3(worldOf(g, ra ? ra : r->h.joint)[3]) - glm::dvec3(worldOf(g, la ? la : l->h.joint)[3]);
        d -= u * glm::dot(d, u);
        if (glm::length(d) > 1e-6)
            right = glm::normalize(d);
    }
    return glm::dmat3(right, u, glm::normalize(glm::cross(u, right)));
}

/** The prop's holster point on the character, made from the class's default when it has none. */
NodeId holsterFor(Graph& g, const Prop& p, NodeId character, const char* cmd)
{
    if (Node* h = named(g, strOf(g, p.node, "holster")); h && characterFor(g, h->id()) == character)
        return h->id();
    const HolsterDefault& hd = preset(p.cls).holster;
    const NodeId joint = jointByNames(g, character, hd.joints);
    if (!joint)
        throw CommandError(std::format("{}: {} has no joint for a {} holster (looked for {})", cmd, g.node(character)->name(), hd.label, hd.joints.front()));
    g.evaluate();
    const glm::dmat3 cf = characterFrame(g, character);
    const glm::dvec3 origin = glm::dvec3(worldOf(g, joint)[3]) + cf * hd.offset;
    const glm::dmat4 frame = frameXZ(origin, cf * hd.x, cf * hd.z);
    Node* n = g.createNode(*types::find(kPointType), g.node(p.root)->name() + "_holster", joint);
    const NodeId id = n->id();
    setPlug(g, id, "role", int64_t(Role::Holster));
    g.evaluate();
    setWorldRotation(g, id, glm::dmat3(frame));
    g.evaluate();
    setWorldPosition(g, id, glm::dvec3(frame[3]));
    setPlug(g, p.node, "holster", g.path(id));
    g.evaluate();
    return id;
}

/** The shoulder pocket an aimed stock goes to: a point under the upper arm, made once per side. */
NodeId shoulderFor(Graph& g, NodeId character, const hand::Hand& h)
{
    const NodeId lower = g.parent(h.joint), arm = lower ? g.parent(lower) : kNoNode;
    if (!arm || !rig::isJoint(*g.node(arm)))
        return kNoNode;
    // On the collarbone, which the aim holds still, so the pocket doesn't swing with the arm.
    const NodeId collar = g.parent(arm);
    const NodeId upper = collar && rig::isJoint(*g.node(collar)) ? collar : arm;
    const std::string name = std::string("propShoulder_") + (h.side == hand::Side::Left ? "l" : "r");
    for (NodeId c : g.children(upper))
        if (g.node(c)->name() == name || g.node(c)->name().ends_with(":" + name))
            return c;
    g.evaluate();
    const glm::dmat3 cf = characterFrame(g, character);
    const double side = h.side == hand::Side::Left ? -1.0 : 1.0;
    // In front of the shoulder joint and toward the chest, where a stock sits.
    const glm::dvec3 at3 = glm::dvec3(worldOf(g, arm)[3]) + cf[2] * 6.0 - cf[0] * side * 4.0 - cf[1] * 3.0;
    Node* n = g.createNode(*types::find(kPointType), name, upper);
    setPlug(g, n->id(), "role", int64_t(Role::Stock));
    g.evaluate();
    setWorldPosition(g, n->id(), at3);
    g.evaluate();
    return n->id();
}

const FlagDef kTimeFlag{"time", "t", ArgType::Float, nullptr, "Seconds the change happens at. Default: now."};
const FlagDef kBlendFlag{"blend", "b", ArgType::Float, 0.2, "Seconds after time the new hold settles over, so nothing pops. 0 snaps.", {}, {}, 0.0};
const FlagDef kSolveFlag{"solve", "sv", ArgType::Bool, true, "Solve the arms over the playback range afterwards (puppeteerPropSolve)."};
const FlagDef kCharacterFlag{"character", "ch", ArgType::String, nullptr, "The character. Default: the one holding the prop, else the selected or only one."};

/** Lets go of values set and keyed at one time (they read the same from their curves), so the solve can sample. */
void releaseHeld(Graph& g)
{
    const double t = g.time();
    g.setTime(t + 1.0, false);
    g.setTime(t, false);
    g.evaluate();
}

json finish(Graph& g, const Prop& p, NodeId grapple, const Args& a, json extra)
{
    releaseHeld(g);
    if (a.get<bool>("solve", true) && grapple)
        extra["solve"] = solveGrapple(g, grapple, a);
    else if (grapple)
    {
        // Unsolved, the scene shows the animation itself rather than a solve of the holds before this edit.
        dropSolve(g, grapple);
        extra["solve"] = nullptr;
    }
    json out = propJson(g, *load(g, p.node));
    for (auto& [k, v] : extra.items())
        out[k] = v;
    return out;
}

// puppeteerPropHold -------------------------------------------------------------------------------------------------

json holdCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropHold";
    Prop p = propArg(g, a, cmd);
    const NodeId character = characterArg(g, a, p, cmd);
    const Preset& pr = preset(p.cls);
    const std::string handName = a.get<std::string>("hand", "main");
    if (handName != "main" && handName != "right" && handName != "left")
        throw CommandError(std::string(cmd) + ": hand is main, right or left");
    const bool right = handName == "main" ? p.rightHanded : handName == "right";
    const int hands = int(a.get<int64_t>("hands", p.twoHanded ? 2 : 1));
    if (hands != 1 && hands != 2)
        throw CommandError(std::string(cmd) + ": hands is 1 or 2");
    const HandRef main = needHand(g, character, right, cmd);
    std::optional<HandRef> off;
    if (hands == 2)
        off = needHand(g, character, !right, cmd);
    const NodeId mainPt = gripPoint(g, p, a.get<std::string>("point", ""), Role::Main, cmd);
    if (!mainPt)
        throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " has no main grip point (puppeteerPropFit)");
    NodeId offPt = kNoNode;
    if (off)
    {
        offPt = gripPoint(g, p, a.get<std::string>("offPoint", ""), Role::Off, cmd);
        if (!offPt)
            offPt = p.point(g, Role::Alt);
        if (!offPt)
            throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " has no off grip point for a second hand");
    }

    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    const double blend = blendArg(a);
    const NodeId grapple = propGrapple(g, character, true);
    const std::set<NodeId> before = handsHolding(g, grapple, p, t);

    // The switch keeps the prop where it is at t; by t + blend it sits in the grip.
    at(g, t);
    const int space = ensureSpace(g, p.root, main.h.joint);
    sp::SwitchOptions so;
    so.time = t;
    so.key = true;
    sp::switchSpace(g, p.root, space, so);
    const double settle = t + blend;
    settleInto(g, p, mainPt, main.h.joint, gripOf(g, main.h, mainPt), t, blend);

    // Contacts: the off hand on its grip, riding the main hand; every other contact of this prop lets go.
    NodeId offContact = kNoNode;
    if (off)
        offContact = contactFor(g, grapple, p, off->h, main.h.joint, offPt);
    for (const PropContact& c : propContacts(g, grapple, p))
        rampWeight(g, c.node, t, blend, c.node == offContact ? 1.0 : 0.0);
    std::set<std::string> using_{g.path(main.h.joint)};
    if (off)
        using_.insert(g.path(off->h.joint));
    for (const NodeId other : props(g))
        if (const std::optional<Prop> op = load(g, other); op && op->node != p.node)
        {
            for (const PropContact& c : propContacts(g, grapple, *op))
                if (Node* s = named(g, c.source); s && using_.count(g.path(s->id())) && weightAt(g, c.node, t) > 1e-6)
                    rampWeight(g, c.node, t, blend, 0.0);
            at(g, settle);
            if (const NodeId h = holderNow(g, *op); h && using_.count(g.path(h)))
                log::write(LogLevel::Warning, std::format("{}: {} is still in {} at frame {:g}; holster or drop it", cmd, g.node(op->root)->name(),
                                                          g.node(h)->name(), settle * fps()));
        }

    // Fingers: the holding hands close over the blend, a hand that let go opens.
    std::set<NodeId> after{main.h.joint};
    if (off)
        after.insert(off->h.joint);
    if (a.get<bool>("fingers", true))
    {
        poseFingers(g, main.h, t, settle, pr.mainPose, true, pr.triggerFinger, &p);
        if (off)
            poseFingers(g, off->h, t, settle, pr.offPose, false, false, nullptr);
        for (NodeId j : before)
            if (!after.count(j))
                if (const NodeId hn = hand::handFor(g, j))
                    if (const std::optional<hand::Hand> h = hand::load(g, hn))
                        poseFingers(g, *h, t, settle, "relaxed", false, false, nullptr);
    }
    json extra = {{"main", g.path(main.h.joint)}, {"mainPoint", g.path(mainPt)}};
    if (off)
    {
        extra["off"] = g.path(off->h.joint);
        extra["offPoint"] = g.path(offPt);
        extra["contact"] = g.path(offContact);
    }
    json out = finish(g, p, grapple, a, extra);
    // The off hand reaches its grip in the solve; its fingers wrap once it is there.
    if (off && a.get<bool>("fingers", true) && a.get<bool>("solve", true))
    {
        at(g, settle);
        wrapFingers(g, off->h, p, pr.offPose, false);
    }
    return out;
}

// puppeteerPropDrop / puppeteerPropHolster -----------------------------------------------------------------------

/** Lets every hand go of the prop over [t, t + blend]: contacts off, fingers relaxed. */
void release(Graph& g, NodeId grapple, const Prop& p, double t, double blend, bool fingers)
{
    const std::set<NodeId> before = handsHolding(g, grapple, p, t);
    for (const PropContact& c : propContacts(g, grapple, p))
        rampWeight(g, c.node, t, blend, 0.0);
    if (fingers)
        for (NodeId j : before)
            if (const NodeId hn = hand::handFor(g, j))
                if (const std::optional<hand::Hand> h = hand::load(g, hn))
                    poseFingers(g, *h, t, t + std::max(blend, 1.0 / fps()), "relaxed", false, false, nullptr);
}

json dropCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropDrop";
    Prop p = propArg(g, a, cmd);
    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    NodeId character = kNoNode;
    for (const Hold& h : holdsOf(g, p))
        if (h.target && !character)
            character = characterFor(g, h.target);
    const NodeId grapple = character ? propGrapple(g, character, false) : kNoNode;
    release(g, grapple, p, t, blendArg(a), a.get<bool>("fingers", true));
    at(g, t);
    sp::SwitchOptions so;
    so.time = t;
    so.key = true;
    sp::switchSpace(g, p.root, worldSpace(g, p.root), so);
    return finish(g, p, grapple, a, json::object());
}

json holsterCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropHolster";
    Prop p = propArg(g, a, cmd);
    const NodeId character = characterArg(g, a, p, cmd);
    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    const double blend = blendArg(a);
    NodeId holster = kNoNode;
    if (a.has("holster"))
    {
        Node& n = nodeNamed(g, a.get<std::string>("holster"), cmd);
        if (n.type().attrIndex("worldMatrix") < 0)
            throw CommandError(std::string(cmd) + ": " + n.path() + " is not a transform");
        holster = n.id();
        setPlug(g, p.node, "holster", g.path(holster));
    }
    else
        holster = holsterFor(g, p, character, cmd);
    const NodeId mainPt = p.point(g, Role::Main);
    if (!mainPt)
        throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " has no main grip point");
    const NodeId grapple = propGrapple(g, character, true);
    release(g, grapple, p, t, blend, a.get<bool>("fingers", true));
    at(g, t);
    sp::SwitchOptions so;
    so.time = t;
    so.key = true;
    sp::switchSpace(g, p.root, ensureSpace(g, p.root, holster), so);
    settleInto(g, p, mainPt, holster, glm::dmat4(1.0), t, blend);
    return finish(g, p, grapple, a, {{"holster", g.path(holster)}});
}

// puppeteerPropAim ----------------------------------------------------------------------------------------------------

AimStyle styleFor(const Preset& pr, const std::string& s, const char* cmd)
{
    if (s.empty() || s == "auto")
        return pr.aim;
    for (AimStyle st : {AimStyle::Turn, AimStyle::Shoulder, AimStyle::Extend, AimStyle::Guard})
        if (s == aimStyleId(st))
            return st;
    throw CommandError(std::string(cmd) + ": style is auto, turn, shoulder, extend or guard");
}

json aimCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropAim";
    Prop p = propArg(g, a, cmd);
    const NodeId character = characterArg(g, a, p, cmd);
    const Preset& pr = preset(p.cls);
    const KeepTime keep(g);
    const double t = a.has("time") ? a.get<double>("time") : g.time();
    const double blend = blendArg(a);
    const NodeId grapple = propGrapple(g, character, true);
    at(g, t + blend);
    const NodeId holder = holderNow(g, p);
    if (!holder)
        throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " isn't in a hand then; hold it first (puppeteerPropHold)");

    // The aims of this prop: one per holding joint, reused when aimed again.
    std::vector<NodeId> mine;
    std::set<std::string> points;
    for (NodeId pt : p.points)
        points.insert(g.path(pt));
    for (NodeId n : gp::aimsOf(g, grapple))
        if (points.count(strOf(g, n, "axis")))
            mine.push_back(n);

    if (a.get<bool>("off", false))
    {
        for (NodeId n : mine)
            rampWeight(g, n, t, blend, 0.0);
        return finish(g, p, grapple, a, json::object());
    }
    if (!a.has("target"))
        throw CommandError(std::string(cmd) + ": give target, the transform to aim at");
    const NodeId target = nodeNamed(g, a.get<std::string>("target"), cmd).id();
    const NodeId axis = p.point(g, Role::Muzzle) ? p.point(g, Role::Muzzle) : p.point(g, Role::Tip);
    if (!axis)
        throw CommandError(std::string(cmd) + ": " + g.node(p.root)->name() + " has no muzzle or tip point to aim");
    const AimStyle style = styleFor(pr, a.get<std::string>("style", "auto"), cmd);
    const NodeId hn = hand::handFor(g, holder);
    const std::optional<hand::Hand> h = hn ? hand::load(g, hn) : std::nullopt;
    NodeId anchor = kNoNode;
    if (style == AimStyle::Shoulder && h)
        anchor = shoulderFor(g, character, *h);
    else if (style != AimStyle::Turn)
    {
        const NodeId lower = g.parent(holder);
        anchor = lower ? g.parent(lower) : kNoNode;
    }
    const NodeId pivot = style == AimStyle::Shoulder && p.point(g, Role::Stock) ? p.point(g, Role::Stock) : p.point(g, Role::Main);

    NodeId aim = kNoNode;
    for (NodeId n : mine)
        if (strOf(g, n, "joint") == g.path(holder))
            aim = n;
    if (!aim)
    {
        json call = {{"args", {g.path(holder), g.path(target)}}, {"axis", g.path(axis)}, {"grapple", g.path(grapple)},
                     {"style", anchor ? aimStyleId(style) : "turn"},    {"blend", 0.0},           {"name", g.node(p.root)->name() + "_aim"}};
        if (anchor)
            call["anchor"] = g.path(anchor);
        const json made = ops::execute("grappleAim", call);
        aim = g.find(made.at("aim").get<std::string>())->id();
        setPlug(g, aim, "weight", 0.0);
        anim::setKey(g, plugOf(*g.node(aim), "weight"), anim::playback().minTime, 0.0);
    }
    setPlug(g, aim, "target", g.path(target));
    setPlug(g, aim, "axis", g.path(axis));
    setPlug(g, aim, "pivot", pivot ? g.path(pivot) : std::string());
    setPlug(g, aim, "anchor", anchor ? g.path(anchor) : std::string());
    setPlug(g, aim, "style", int64_t(anchor ? style : AimStyle::Turn));
    setPlug(g, aim, "drop", style == AimStyle::Guard ? pr.drop : 0.0);
    if (anchor && (style == AimStyle::Extend || style == AimStyle::Guard))
    {
        double arm = 0.0;
        for (NodeId j = holder; j && j != anchor; j = g.parent(j))
            arm += glm::length(glm::dvec3(worldOf(g, j)[3]) - glm::dvec3(worldOf(g, g.parent(j))[3]));
        setPlug(g, aim, "reach", pr.reach * arm);
    }
    const double w = std::clamp(a.get<double>("weight", 1.0), 0.0, 1.0);
    rampWeight(g, aim, t, blend, w);
    for (NodeId n : mine)
        if (n != aim)
            rampWeight(g, n, t, blend, 0.0);
    return finish(g, p, grapple, a, {{"aim", g.path(aim)}, {"style", aimStyleId(anchor ? style : AimStyle::Turn)}, {"target", g.path(target)}});
}

// puppeteerPropUnreal -------------------------------------------------------------------------------------------------

/** World matrices the gun, right and left ik bones follow each frame: the holding hand, then each hand. */
json unrealCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropUnreal";
    Prop p = propArg(g, a, cmd);
    const NodeId character = characterArg(g, a, p, cmd);
    const KeepTime keep(g);
    g.evaluate();
    NodeId bone = kNoNode;
    if (a.has("bone"))
    {
        bone = jointByNames(g, character, {a.get<std::string>("bone")});
        if (!bone)
            throw CommandError(std::format("{}: {} has no bone {}", cmd, g.node(character)->name(), a.get<std::string>("bone")));
    }
    else
    {
        bone = holderNow(g, p);
        if (!bone)
            bone = jointByNames(g, character, {p.rightHanded ? "hand_r" : "hand_l"});
        if (!bone)
            if (const std::optional<HandRef> h = handOf(g, character, p.rightHanded))
                bone = h->h.joint;
        if (!bone)
            throw CommandError(std::string(cmd) + ": give bone, the joint the prop attaches to");
    }
    std::string bareBone = g.node(bone)->name();
    if (const size_t c = bareBone.rfind(':'); c != std::string::npos)
        bareBone = bareBone.substr(c + 1);
    std::string socket = a.get<std::string>("socket", "");
    if (socket.empty())
        socket = bareBone.ends_with("_r") ? "weapon_r" : bareBone.ends_with("_l") ? "weapon_l" : bareBone + "_prop";

    const anim::Playback& pb = anim::playback();
    const double start = a.has("start") ? a.get<double>("start") : pb.minTime, end = a.has("end") ? a.get<double>("end") : pb.maxTime;
    const long long frames = std::max<long long>(0, std::llround((end - start) * fps()));
    // The socket's offset: the prop against the bone where the grip has settled, the last frame of the first run of frames
    // it is held; how far it strays from that over the held frames (the ease into the grip included) is reported.
    std::vector<glm::dmat4> held;
    std::optional<glm::dmat4> offset;
    bool runOver = false;
    for (long long f = 0; f <= frames; ++f)
    {
        at(g, start + double(f) / fps());
        if (holderNow(g, p) != bone)
        {
            runOver = runOver || !held.empty();
            continue;
        }
        held.push_back(glm::inverse(rigidOf(worldOf(g, bone))) * rigidOf(worldOf(g, p.root)));
        if (!runOver)
            offset = held.back();
    }
    const int heldFrames = int(held.size());
    double strayCm = 0.0, strayDeg = 0.0;
    for (const glm::dmat4& rel : held)
    {
        strayCm = std::max(strayCm, glm::length(glm::dvec3(rel[3]) - glm::dvec3((*offset)[3])));
        const glm::dquat q0 = glm::quat_cast(glm::dmat3(*offset)), q1 = glm::quat_cast(glm::dmat3(rel));
        strayDeg = std::max(strayDeg, glm::degrees(2.0 * std::acos(std::clamp(std::abs(glm::dot(q0, q1)), 0.0, 1.0))));
    }
    if (!offset)
    {
        at(g, g.time());
        offset = glm::inverse(rigidOf(worldOf(g, bone))) * rigidOf(worldOf(g, p.root));
    }
    for (const auto& [name, type, value] : {std::tuple{pins::kSocketAttr, PlugType::String, PlugValue(socket)},
                                            std::tuple{pins::kSocketBoneAttr, PlugType::String, PlugValue(bareBone)},
                                            std::tuple{pins::kSocketOffsetAttr, PlugType::Matrix, PlugValue(*offset)}})
    {
        if (g.node(p.root)->type().attrIndex(name) < 0)
            g.addAttr(p.root, {.name = name, .type = type, .defaultValue = type == PlugType::Matrix ? PlugValue(glm::dmat4(1.0)) : PlugValue(std::string()),
                               .flags = AttrInput | AttrStorable,
                               .doc = "Unreal: how the prop attaches to its character's skeletal mesh (puppeteerPropUnreal)."});
        g.setValue({p.root, g.node(p.root)->type().attrIndex(name), -1}, value);
    }
    json out = {{"prop", g.path(p.root)}, {"socket", socket}, {"bone", bareBone}, {"heldFrames", heldFrames},
                {"strayCm", strayCm}, {"strayDeg", strayDeg}};
    if (heldFrames == 0)
        log::write(LogLevel::Warning, std::format("{}: {} is never held by {} in the range; the socket uses the pose now", cmd, g.path(p.root), bareBone));
    else if (strayCm > 0.5 || strayDeg > 1.0)
        log::write(LogLevel::Warning, std::format("{}: {} moves against {} while held (up to {:.1f} cm, {:.1f} deg); a socket holds one offset",
                                                  cmd, g.path(p.root), bareBone, strayCm, strayDeg));

    // Optional ik bones (UE5's ik_hand_gun, ik_hand_r, ik_hand_l): keyed to follow the hands every frame of the range.
    if (a.get<bool>("ikBones", false))
    {
        const std::string gunName = a.get<std::string>("gunBone", "ik_hand_gun"), rName = a.get<std::string>("rightBone", "ik_hand_r"),
                          lName = a.get<std::string>("leftBone", "ik_hand_l");
        const NodeId gun = jointByNames(g, character, {gunName}), ikR = jointByNames(g, character, {rName}), ikL = jointByNames(g, character, {lName});
        std::string missing;
        for (const auto& [id, n] : {std::pair{gun, gunName}, std::pair{ikR, rName}, std::pair{ikL, lName}})
            if (!id)
                missing += (missing.empty() ? "" : ", ") + n;
        if (!missing.empty())
            throw CommandError(std::format("{}: the skeleton has no {}; ikBones needs them (or name yours with gunBone, rightBone, leftBone)", cmd, missing));
        const std::optional<HandRef> hr = handOf(g, character, true), hl = handOf(g, character, false);
        if (!hr || !hl)
            throw CommandError(std::string(cmd) + ": ikBones needs both hands set up (puppeteerHandSetup)");
        struct Track
        {
            NodeId bone;
            std::vector<double> t;
            std::vector<glm::dvec3> tr, ro;
        };
        std::vector<Track> tracks{{gun}, {ikR}, {ikL}};
        for (long long f = 0; f <= frames; ++f)
        {
            const double time = start + double(f) / fps();
            at(g, time);
            const NodeId holding = holderNow(g, p);
            const glm::dmat4 gunWorld = rigidOf(worldOf(g, holding ? holding : hr->h.joint));
            const glm::dmat4 want[3] = {gunWorld, rigidOf(worldOf(g, hr->h.joint)), rigidOf(worldOf(g, hl->h.joint))};
            // Each bone's parent as it will be: the gun bone's children follow its new pose.
            glm::dmat4 placed[3];
            for (int k = 0; k < 3; ++k)
            {
                const NodeId b = tracks[size_t(k)].bone;
                glm::dmat4 parent = parentWorldOf(g, b);
                if (k > 0 && g.parent(b) == gun)
                    parent = placed[0];
                placed[k] = want[k];
                const glm::dmat4 local = glm::inverse(parent) * want[k];
                const glm::dmat3 orient = orientOf(g, b);
                const glm::dmat3 r = glm::transpose(orient) * math::rotationOf(local);
                Track& tk = tracks[size_t(k)];
                const glm::dvec3 near = tk.ro.empty() ? vecOf(g, b, Transform::kRotate) : tk.ro.back();
                tk.t.push_back(time);
                tk.tr.push_back(glm::dvec3(local[3]));
                tk.ro.push_back(eulerNear(r, rotateOrderOf(g, b), near));
            }
        }
        for (const Track& tk : tracks)
        {
            g.setValue({tk.bone, Transform::kRotationInterpolation, -1}, int64_t(RotationInterpolation::Euler));
            for (const auto& [attr, values] : {std::pair{Transform::kTranslate, &tk.tr}, std::pair{Transform::kRotate, &tk.ro}})
                for (int c = 0; c < 3; ++c)
                {
                    anim::Curve curve;
                    for (size_t i = 0; i < tk.t.size(); ++i)
                        curve.keys.push_back({.time = tk.t[i], .value = (*values)[i][c], .in = anim::Tangent::Linear, .out = anim::Tangent::Linear});
                    const NodeId cn = anim::setKey(g, {tk.bone, attr, c}, curve.keys.front().time, curve.keys.front().value);
                    anim::setCurve(g, cn, curve);
                }
        }
        out["ikBones"] = {g.path(gun), g.path(ikR), g.path(ikL)};
        out["ikFrames"] = frames + 1;
    }
    return out;
}

// Registration ------------------------------------------------------------------------------------------------------

void registerTypes(ext::Context& ctx)
{
    const NodeType* transform = types::find("transform");
    NodeType prop{.name = kPropType,
                  .base = transform,
                  .dag = true,
                  .doc = "Marks its parent as a prop or weapon: its class, which hand holds it, whether two do, and its grip, "
                         "muzzle, tip and stock points (puppeteerPropPoint children). Made by puppeteerPropSetup.",
                  .icon = "sword",
                  .create = transform->create};
    prop.ownAttrs = {
        {.name = "propClass", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable, .enumNames = classIds(),
         .doc = "What kind of prop: sets the default grips, hand poses, two-hand hold, holster and aim."},
        {.name = "hand", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable, .enumNames = {"right", "left"},
         .doc = "The hand that holds it by default."},
        {.name = "twoHanded", .type = PlugType::Bool, .defaultValue = false, .flags = AttrInput | AttrStorable, .doc = "Held in both hands by default."},
        text("holster", "The point on the character it is put away at (made by puppeteerPropHolster)."),
    };
    NodeType point{.name = kPointType,
                   .base = transform,
                   .dag = true,
                   .doc = "A point on a prop (a grip, the muzzle or tip, the stock) or on a character (a holster). Grips: +X runs "
                          "along the handle toward the thumb, +Z is the way the palm faces. Muzzle and tip: +X is aimed.",
                   .icon = "point",
                   .create = transform->create};
    point.ownAttrs = {
        {.name = "role", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable, .enumNames = roleIds(),
         .doc = "main, off and alt are grips; muzzle and tip are aimed; stock goes to the shoulder; holster holds a put-away prop."},
        number("radius", 1.6, "Grips: the handle's radius, cm, which sets how far the palm sits from it."),
    };
    for (NodeType* t : {&prop, &point})
    {
        // Points live in their parent's space; the prop node rides its root with no offset.
        relocate::registerNeutral(t->name);
        types::registerType(ctx, std::move(*t));
    }
}

json pointCmd(const Args& a)
{
    Graph& g = Graph::current();
    const char* cmd = "puppeteerPropPoint";
    Prop p = propArg(g, a, cmd);
    const std::string roleName = a.get<std::string>("role", "alt");
    const std::optional<Role> role = roleFromId(roleName);
    if (!role || *role == Role::Holster)
        throw CommandError(std::string(cmd) + ": role is main, off, alt, muzzle, tip or stock");
    g.evaluate();
    PointDef d;
    d.role = *role;
    d.name = a.get<std::string>("name", roleName);
    d.radius = a.get<double>("radius", 1.6);
    const NodeId like = p.point(g, *role == Role::Alt ? Role::Main : *role);
    d.frame = like ? pointLocal(g, p, like) : glm::dmat4(1.0);
    if (a.has("position"))
        d.frame[3] = glm::dvec4(glm::dvec3(glm::inverse(rigidOf(worldOf(g, p.root))) * glm::dvec4(jsonVec(a.flags.at("position")), 1.0)), 1.0);
    const NodeId id = writePoint(g, p, d);
    return {{"point", g.path(id)}, {"role", roleId(*role)}};
}

void registerCommands(ext::Context& ctx)
{
    using T = ArgType;
    ctx.registerCommand({
        .name = "puppeteerPropSetup",
        .doc = "Marks a mesh or group as a prop or weapon of a class and fits its points to the mesh: grips (main, off, "
               "alternates), the muzzle or tip that aims, and the stock. Points are transforms under the prop; move and turn them "
               "in the viewport. Already a prop: changes its class or hand and fits again. Returns the prop.",
        .positionalDoc = "mesh or group (default the selection)",
        .flags = {
            {"kind", "k", T::String, nullptr, classFlagDoc(), classIds()},
            {"hand", "hd", T::String, nullptr, "right or left: the hand that holds it by default.", {"right", "left"}},
            {"twoHanded", "th", T::Bool, nullptr, "Held in both hands by default. Default by class."},
            {"fit", "ft", T::Bool, true, "Fit the points to the mesh."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> roots;
            for (const std::string& s : objectArgs(a))
                roots.push_back(nodeNamed(g, s, "puppeteerPropSetup").id());
            if (roots.empty())
                for (NodeId s : g.selection())
                    if (!propFor(g, s) || std::find(roots.begin(), roots.end(), s) == roots.end())
                        roots.push_back(s);
            if (roots.empty())
                throw CommandError("puppeteerPropSetup: give or select the prop's mesh or group");
            json out = json::array();
            for (NodeId r : roots)
            {
                NodeId pn = propFor(g, r);
                if (pn && g.parent(pn) != r && g.node(r)->type().isA(kPointType))
                    r = g.parent(pn);
                if (pn && g.parent(pn) != r)
                    r = g.parent(pn);
                if (characterFor(g, r) && !pn)
                    throw CommandError("puppeteerPropSetup: " + g.path(r) + " is part of a character");
                if (g.node(r)->type().attrIndex("worldMatrix") < 0)
                    throw CommandError("puppeteerPropSetup: " + g.path(r) + " is not a transform");
                const bool made = !pn;
                if (made)
                    pn = g.createNode(*types::find(kPropType), g.node(r)->name() + "_prop", r)->id();
                const Class cls = classArg(a, "puppeteerPropSetup", made ? Class::Generic : load(g, pn)->cls);
                const Preset& pr = preset(cls);
                const bool classChanged = made || a.has("kind");
                setPlug(g, pn, "propClass", int64_t(cls));
                if (a.has("hand"))
                    setPlug(g, pn, "hand", int64_t(a.get<std::string>("hand") == "left" ? 1 : 0));
                if (a.has("twoHanded"))
                    setPlug(g, pn, "twoHanded", a.get<bool>("twoHanded"));
                else if (classChanged)
                    setPlug(g, pn, "twoHanded", pr.twoHanded);
                Prop p = *load(g, pn);
                json fitted = nullptr;
                if (a.get<bool>("fit", true) && (classChanged || p.points.empty()))
                    fitted = fitProp(g, p);
                json j = propJson(g, p);
                j["fit"] = fitted;
                out.push_back(j);
            }
            g.evaluate();
            return out.size() == 1 ? out.front() : out;
        },
        .examples = {"puppeteerPropSetup(\"rifle\", kind=\"rifle\")", "puppeteerPropSetup(\"sword\", kind=\"sword2h\")"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropFit",
        .doc = "Fits a prop's points to its mesh again by its class (points the fit doesn't make are kept). Returns how they were found.",
        .positionalDoc = "prop (default the selection)",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Prop p = propArg(g, a, "puppeteerPropFit");
            json out = fitProp(g, p);
            out["prop"] = propJson(g, p);
            return out;
        },
    });

    ctx.registerCommand({
        .name = "puppeteerPropPoint",
        .doc = "Adds a point to a prop: another grip (alt), or a muzzle, tip or stock. It starts on the point of that role, or "
               "the main grip, or at position. Returns the point.",
        .positionalDoc = "prop (default the selection)",
        .flags = {{"role", "r", T::String, "alt", "main, off, alt, muzzle, tip or stock.", {"main", "off", "alt", "muzzle", "tip", "stock"}},
                  {"name", "n", T::String, nullptr, "Name (after the prop's). Default: the role."},
                  {"position", "p", T::Vec3, nullptr, "World point to put it at."},
                  {"radius", "rd", T::Float, 1.6, "Grips: the handle's radius, cm.", {}, {}, 0.0}},
        .run = pointCmd,
    });

    ctx.registerCommand({
        .name = "puppeteerPropHold",
        .doc = "Puts a prop in a character's hand from time on: it switches into the hand's space there (no pop) and settles "
               "into the grip by time + blend, the hand closing round it. hands=2 puts the other hand on the off grip too, held by "
               "the solve, which shares the reach between both arms. A hand that held it before and doesn't now lets go. Use it "
               "again later to hand it to the other hand or change grip. Returns the prop with its holds.",
        .positionalDoc = "prop (default the selection)",
        .flags = {
            kCharacterFlag,
            {"hand", "hd", T::String, "main", "main (the prop's), right or left.", {"main", "right", "left"}},
            {"hands", "n", T::Int, nullptr, "1 or 2. Default: the prop's two-handed setting.", {}, {}, 1.0, 2.0},
            {"point", "pt", T::String, nullptr, "The grip point the hand takes, by name or role. Default: main."},
            {"offPoint", "op", T::String, nullptr, "With two hands: the other hand's grip. Default: off."},
            {"fingers", "fi", T::Bool, true, "Pose and wrap the fingers round the prop."},
            kTimeFlag, kBlendFlag, kSolveFlag,
            {"start", "", T::Int, nullptr, "Solve from this frame. Default: the playback start."},
            {"end", "", T::Int, nullptr, "Solve to this frame."},
        },
        .run = holdCmd,
        .examples = {"puppeteerPropHold(\"sword\", hands=2, time=0.5)", "puppeteerPropHold(\"pistol\", hand=\"left\", time=2.0, blend=0.25)"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropDrop",
        .doc = "Lets go of a prop at time: it stays in the world where it is, the hands open over blend. Returns the prop.",
        .positionalDoc = "prop (default the selection)",
        .flags = {kTimeFlag, kBlendFlag, kSolveFlag, {"fingers", "fi", T::Bool, true, "Open the hands that held it."}},
        .run = dropCmd,
    });

    ctx.registerCommand({
        .name = "puppeteerPropHolster",
        .doc = "Puts a prop away at time: it switches to its holster on the character (made from the class's default spot, "
               "back, hip or thigh, the first time; move it to taste) and settles there by time + blend while the hands let go. "
               "puppeteerPropHold draws it again. Returns the prop.",
        .positionalDoc = "prop (default the selection)",
        .flags = {kCharacterFlag,
                  nodeFlag({"holster", "hs", T::String, nullptr, "A transform on the character to put it at instead."}, "transform"),
                  kTimeFlag, kBlendFlag, kSolveFlag, {"fingers", "fi", T::Bool, true, "Open the hands that held it."}},
        .run = holsterCmd,
    });

    ctx.registerCommand({
        .name = "puppeteerPropAim",
        .doc = "Aims a held prop's muzzle or tip at a target from time on, weight blending in over blend; the arms and body "
               "follow in the solve. style auto takes the class's: shoulder (rifles, shotguns, launchers: the stock to the "
               "shoulder), extend (pistols, bows: held out toward the target), guard (blades: held out, dropped below the line), "
               "turn (turns where it is). off=True blends the aim out. Returns the prop.",
        .positionalDoc = "prop (default the selection)",
        .flags = {kCharacterFlag,
                  nodeFlag({"target", "tg", T::String, nullptr, "The transform to aim at (a locator)."}, "transform"),
                  {"style", "s", T::String, "auto", "auto, turn, shoulder, extend or guard.", {"auto", "turn", "shoulder", "extend", "guard"}},
                  {"weight", "w", T::Float, 1.0, "How fully to aim.", {}, {}, 0.0, 1.0},
                  {"off", "", T::Bool, false, "Blend the prop's aims out instead."},
                  kTimeFlag, kBlendFlag, kSolveFlag,
                  {"start", "", T::Int, nullptr, "Solve from this frame."},
                  {"end", "", T::Int, nullptr, "Solve to this frame."}},
        .run = aimCmd,
        .examples = {"puppeteerPropAim(\"rifle\", target=\"enemy\", time=1.0)"},
    });

    ctx.registerCommand({
        .name = "puppeteerPropSolve",
        .doc = "Solves the arms and body holding props over a range onto the character's props layer: off hands held on their "
               "grips with both arms sharing the reach, aims turned onto their targets. Holds and aims do this themselves unless "
               "solve=False. Returns {'layer', 'frames', 'contactError'} (cm).",
        .positionalDoc = "prop or character (default the selection)",
        .flags = {kCharacterFlag, {"start", "", T::Int, nullptr, "First frame. Default: the playback start."}, {"end", "", T::Int, nullptr, "Last frame."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            NodeId character = kNoNode;
            for (const std::string& s : objectArgs(a))
                if (!character)
                    character = characterFor(g, nodeNamed(g, s, "puppeteerPropSolve").id());
            if (!character)
                character = characterArg(g, a, propArg(g, a, "puppeteerPropSolve"), "puppeteerPropSolve");
            const NodeId grapple = propGrapple(g, character, false);
            if (!grapple)
                throw CommandError("puppeteerPropSolve: " + g.node(character)->name() + " holds no props");
            return solveGrapple(g, grapple, a);
        },
    });

    ctx.registerCommand({
        .name = "puppeteerPropInfo",
        .doc = "A prop's class, points (world positions), holds over time (who or what holds it from when) and its holder now.",
        .positionalDoc = "prop (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Prop p = propArg(g, a, "puppeteerPropInfo");
            g.evaluate();
            return propJson(g, p);
        },
    });

    ctx.registerCommand({
        .name = "puppeteerPropClasses",
        .doc = "The prop classes and what each sets: id, label, two-handed, hand poses, aim style and holster spot.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const Preset& p : presets())
                out.push_back({{"id", p.id}, {"label", p.label}, {"icon", p.icon}, {"twoHanded", p.twoHanded}, {"mainPose", p.mainPose},
                               {"offPose", p.offPose}, {"aim", aimStyleId(p.aim)}, {"holster", p.holster.label}});
            return out;
        },
    });

    ctx.registerCommand({
        .name = "puppeteerPropUnreal",
        .doc = "Sets a prop up for Unreal: it attaches to a bone of the character's skeletal mesh (by default the hand holding "
               "it, hand_r) through a socket with the offset it is held at, which the Unreal link makes on the mesh when it sends "
               "the prop; the character's own bones carry the animation. With ikBones, also keys the skeleton's hand IK bones "
               "(by default UE5's ik_hand_gun, ik_hand_r, ik_hand_l) to follow the hands every frame, for projects whose "
               "locomotion reads them. Returns the socket, bone, and how far the prop strays against the bone while held.",
        .positionalDoc = "prop (default the selection)",
        .flags = {kCharacterFlag,
                  {"bone", "bn", T::String, nullptr, "The bone it attaches to. Default: the hand holding it now, else hand_r (hand_l left-handed)."},
                  {"socket", "sk", T::String, nullptr, "The socket's name. Default: weapon_r or weapon_l by the bone's side."},
                  {"ikBones", "ik", T::Bool, false, "Key the hand IK bones to follow the hands. Off by default; needs the bones."},
                  {"gunBone", "", T::String, "ik_hand_gun", "ikBones: the bone that follows the holding hand."},
                  {"rightBone", "", T::String, "ik_hand_r", "ikBones: the bone that follows the right hand."},
                  {"leftBone", "", T::String, "ik_hand_l", "ikBones: the bone that follows the left hand."},
                  {"start", "st", T::Float, nullptr, "Seconds; default the playback start."},
                  {"end", "et", T::Float, nullptr, "Seconds; default the playback end."}},
        .run = unrealCmd,
        .examples = {"puppeteerPropUnreal(\"rifle\")", "puppeteerPropUnreal(\"rifle\", ikBones=True)"},
    });
}
} // namespace

void registerProps(ext::Context& ctx)
{
    registerTypes(ctx);
    registerCommands(ctx);
}
} // namespace puppeteer::prop

namespace puppeteer
{
void registerProps(lila::ext::Context& ctx) { prop::registerProps(ctx); }
} // namespace puppeteer
