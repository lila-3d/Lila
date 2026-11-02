#include "scene/Turntable.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/NodeBounds.h"
#include "scene/ViewState.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_set>

namespace lila::scene::turntable
{
namespace
{
using T = ArgType;

std::mutex& registryMutex()
{
    static std::mutex m;
    return m;
}

/** Every Turntable object alive, in creation order; one that is in an undo step is alive but not in its graph. */
std::vector<Turntable*>& live()
{
    static std::vector<Turntable*> v;
    return v;
}

/** The id is authoritative while the node lives; the name binds entries read from a file. */
struct Members final : Data
{
    struct Entry
    {
        mutable NodeId id = kNoNode;
        mutable std::string name;
    };
    std::vector<Entry> entries;

    const char* kind() const override { return kMembersKind; }
    json toJson() const override
    {
        const Graph& g = Graph::current();
        json names = json::array();
        for (const Entry& e : entries)
        {
            const Node* n = e.id != kNoNode ? g.node(e.id) : nullptr;
            if (n)
                names.push_back(n->path());
            else if (e.id == kNoNode)
                names.push_back(e.name);
        }
        return {{"kind", kind()}, {"members", std::move(names)}};
    }
    size_t bytes() const override { return 64 + entries.size() * 48; }

    static DataPtr fromJson(const json& j)
    {
        auto out = std::make_shared<Members>();
        for (const json& m : j.value("members", json::array()))
            if (m.is_string())
                out->entries.push_back({kNoNode, m.get<std::string>()});
        return out;
    }

    std::vector<NodeId> resolve(const Graph& g, const std::string& ns) const
    {
        std::vector<NodeId> out;
        for (const Entry& e : entries)
        {
            const Node* n = nullptr;
            if (e.id != kNoNode)
                n = g.node(e.id); // a deleted member stays unbound
            else
                try
                {
                    if (!ns.empty())
                        n = g.find(ns + ":" + e.name);
                    if (!n)
                        n = g.find(e.name);
                }
                catch (const std::exception&)
                {
                    continue; // ambiguous until the name is made unique
                }
            if (!n || !n->type().dag)
                continue;
            e.id = n->id();
            e.name = n->path();
            out.push_back(n->id());
        }
        return out;
    }
};

double wrap(double degrees)
{
    double a = std::fmod(degrees, 360.0);
    return a < 0.0 ? a + 360.0 : a;
}

/** Shortest signed difference to - from, in (-180, 180]. */
double delta(double from, double to)
{
    double d = std::fmod(to - from, 360.0);
    if (d > 180.0)
        d -= 360.0;
    else if (d <= -180.0)
        d += 360.0;
    return d;
}

double smootherstep(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

/** The turntable's world frame from its own attrs, so a pivot moved this frame counts before the node evaluates. */
glm::dmat4 frameOf(const Graph& g, const Node& t)
{
    glm::dmat4 parent(1.0);
    if (NodeId p = g.parent(t.id()); p != kNoNode)
        if (const Node* pn = g.node(p); pn && pn->type().dag)
            parent = pn->as<glm::dmat4>(Transform::kWorldMatrix);
    return parent * Transform::localMatrix(t);
}

void add(std::vector<NodeId>& v, NodeId id)
{
    if (std::find(v.begin(), v.end(), id) == v.end())
        v.push_back(id);
}

Node& nodeArg(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = nullptr;
    try
    {
        n = g.find(name);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    if (!n)
        throw CommandError(std::string(cmd) + ": no node called '" + name + "'");
    return *n;
}

NodeId lastUsed = kNoNode;

/** The turntable a command acts on: the named one, else the selected one or the one a selected node is on, else the last used, else the first. */
Turntable& target(Graph& g, const Args& a, const char* cmd)
{
    if (!a.positional.empty() && a.positional[0].is_string())
    {
        Node& n = nodeArg(g, a.positional[0].get<std::string>(), cmd);
        if (Turntable* t = get(g, n.id()))
            return lastUsed = t->id(), *t;
        if (NodeId on = turntableOf(g, n.id()); on != kNoNode)
            return lastUsed = on, *get(g, on);
        throw CommandError(std::string(cmd) + ": " + n.path() + " is not a turntable or on one");
    }
    for (NodeId id : g.selection())
    {
        if (Turntable* t = get(g, id))
            return lastUsed = t->id(), *t;
        if (NodeId on = turntableOf(g, id); on != kNoNode)
            return lastUsed = on, *get(g, on);
    }
    if (Turntable* t = get(g, lastUsed))
        return *t;
    const std::vector<Turntable*> every = all(g);
    if (every.empty())
        throw CommandError(std::string(cmd) + ": there is no turntable; put something on one first");
    lastUsed = every.front()->id();
    return *every.front();
}

std::vector<NodeId> objectsArg(Graph& g, const json& list, const char* cmd)
{
    std::vector<NodeId> out;
    auto one = [&](const json& v) {
        if (v.is_string())
            if (Node& n = nodeArg(g, v.get<std::string>(), cmd); n.type().dag && !isTurntable(n))
                add(out, n.id());
    };
    for (const json& v : list)
        if (v.is_array())
            for (const json& x : v)
                one(x);
        else
            one(v);
    return out;
}

std::vector<NodeId> selectedObjects(const Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.selection())
        if (const Node* n = g.node(id); n && n->type().dag && !isTurntable(*n))
            add(out, id);
    return out;
}

json namesOf(const Graph& g, const std::vector<NodeId>& ids)
{
    json out = json::array();
    for (NodeId id : ids)
        if (const Node* n = g.node(id))
            out.push_back(n->path());
    return out;
}

const char* kAttrNames[] = {"secondsPerTurn", "direction", "ease", "restAngle", "stepAngle", "lightsFollow", "holdWhileEditing", "resumeAfter", "includeInRenders"};

/** Sets the attrs given as flags, undoably. */
void applyFlags(Graph& g, Node& t, const Args& a)
{
    for (const char* name : kAttrNames)
    {
        if (!a.has(name))
            continue;
        const int attr = t.type().attrIndex(name);
        const json& v = a.flags[name];
        const AttrDef& d = t.type().attrs[size_t(attr)];
        if (d.type == PlugType::Bool)
            g.setValue({t.id(), attr, -1}, v.get<bool>());
        else if (d.type == PlugType::Enum)
        {
            int64_t index = 0;
            if (v.is_string())
            {
                auto it = std::find(d.enumNames.begin(), d.enumNames.end(), v.get<std::string>());
                if (it == d.enumNames.end())
                    throw CommandError("turntable: " + std::string(name) + " is one of counterClockwise, clockwise");
                index = it - d.enumNames.begin();
            }
            else
                index = v.get<int64_t>();
            g.setValue({t.id(), attr, -1}, index);
        }
        else
            g.setValue({t.id(), attr, -1}, v.get<double>());
    }
    if (a.has("pivot"))
    {
        const json& p = a.flags["pivot"];
        g.setValue({t.id(), Transform::kTranslate, -1}, glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()));
    }
}

Turntable& create(Graph& g, const std::vector<NodeId>& list, const std::string& name)
{
    const NodeType* type = types::find(kTypeName);
    Node* n = g.createNode(*type, name.empty() ? "turntable1" : name);
    if (auto c = footprintCentre(g, list))
        g.setValue({n->id(), Transform::kTranslate, -1}, *c);
    setMembers(g, n->id(), list);
    lastUsed = n->id();
    return static_cast<Turntable&>(*n);
}

json query(Graph& g, Turntable& t)
{
    json out = {{"name", t.path()},
                {"members", namesOf(g, members(g, t.id()))},
                {"angle", t.angle},
                {"pitch", t.pitch},
                {"playing", t.playing},
                {"held", t.held},
                {"speedScale", t.speedScale},
                {"pivot", {pivot(g, t.id()).x, pivot(g, t.id()).y, pivot(g, t.id()).z}},
                {"axis", {axis(g, t.id()).x, axis(g, t.id()).y, axis(g, t.id()).z}}};
    for (const char* name : kAttrNames)
    {
        const int attr = t.type().attrIndex(name);
        const AttrDef& d = t.type().attrs[size_t(attr)];
        if (d.type == PlugType::Bool)
            out[name] = t.as<bool>(attr);
        else if (d.type == PlugType::Enum)
            out[name] = d.enumNames[size_t(t.as<int64_t>(attr))];
        else
            out[name] = t.as<double>(attr);
    }
    return out;
}
} // namespace

Turntable::Turntable()
{
    std::scoped_lock lock(registryMutex());
    live().push_back(this);
}

Turntable::~Turntable()
{
    std::scoped_lock lock(registryMutex());
    std::erase(live(), this);
}

double now()
{
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

bool isTurntable(const Node& n) { return n.type().isA(kTypeName); }

std::vector<Turntable*> all(const Graph& g)
{
    std::vector<Turntable*> out;
    std::scoped_lock lock(registryMutex());
    for (Turntable* t : live())
        if (t->id() != kNoNode && g.node(t->id()) == t)
            out.push_back(t);
    return out;
}

Turntable* get(const Graph& g, NodeId id)
{
    Node* n = id != kNoNode ? g.node(id) : nullptr;
    return n && isTurntable(*n) ? static_cast<Turntable*>(n) : nullptr;
}

std::vector<NodeId> members(const Graph& g, NodeId turntable)
{
    const Node* n = g.node(turntable);
    if (!n || !isTurntable(*n))
        return {};
    const DataPtr& d = n->as<DataPtr>(Turntable::kMembers);
    const auto* m = d ? dynamic_cast<const Members*>(d.get()) : nullptr;
    return m ? m->resolve(g, n->nameSpace()) : std::vector<NodeId>{};
}

void setMembers(Graph& g, NodeId turntable, const std::vector<NodeId>& list)
{
    auto d = std::make_shared<Members>();
    for (NodeId id : list)
        if (const Node* n = g.node(id))
            d->entries.push_back({id, n->path()});
    g.setValue({turntable, Turntable::kMembers, -1}, DataPtr(std::move(d)));
}

NodeId turntableOf(const Graph& g, NodeId node)
{
    const std::vector<Turntable*> every = all(g);
    if (every.empty())
        return kNoNode;
    for (NodeId cur = node; cur != kNoNode; cur = g.parent(cur))
        for (Turntable* t : every)
        {
            const std::vector<NodeId> m = members(g, t->id());
            if (std::find(m.begin(), m.end(), cur) != m.end())
                return t->id();
        }
    return kNoNode;
}

glm::dvec3 axis(const Graph& g, NodeId turntable)
{
    const Node* n = g.node(turntable);
    if (!n)
        return sceneUp();
    const glm::dvec3 a = glm::dmat3(frameOf(g, *n)) * sceneUp();
    return glm::length(a) > 1e-12 ? glm::normalize(a) : sceneUp();
}

glm::dvec3 pivot(const Graph& g, NodeId turntable)
{
    const Node* n = g.node(turntable);
    return n ? glm::dvec3(frameOf(g, *n)[3]) : glm::dvec3(0.0);
}

glm::dmat4 turnBy(const Graph& g, NodeId turntable, double degrees)
{
    const glm::dvec3 p = pivot(g, turntable);
    return glm::translate(glm::dmat4(1.0), p) * glm::rotate(glm::dmat4(1.0), glm::radians(degrees), axis(g, turntable)) *
           glm::translate(glm::dmat4(1.0), -p);
}

glm::dmat4 liveTurn(const Graph& g, const Turntable& t)
{
    const glm::dmat4 spin = turnBy(g, t.id(), t.angle);
    if (std::abs(t.pitch) < 1e-9)
        return spin;
    const glm::dvec3 c = t.pitchCentre;
    return glm::translate(glm::dmat4(1.0), c) * glm::rotate(glm::dmat4(1.0), glm::radians(t.pitch), t.pitchAxis) *
           glm::translate(glm::dmat4(1.0), -c) * spin;
}

double cruiseSpeed(const Turntable& t)
{
    const double seconds = std::max(t.as<double>(Turntable::kSecondsPerTurn), 0.1);
    const double sign = t.as<int64_t>(Turntable::kDirection) == 1 ? -1.0 : 1.0;
    return sign * 360.0 / seconds * t.speedScale;
}

void sync(Graph& g)
{
    std::unordered_map<NodeId, glm::dmat4> turns;
    const std::vector<Turntable*> every = all(g);
    if (!every.empty() && !viewState::isExport())
    {
        std::unordered_set<NodeId> onAny;
        std::vector<std::pair<Turntable*, std::vector<NodeId>>> lists;
        for (Turntable* t : every)
        {
            lists.emplace_back(t, members(g, t->id()));
            onAny.insert(lists.back().second.begin(), lists.back().second.end());
        }
        auto underAny = [&](NodeId id, const auto& set) {
            for (NodeId p = g.parent(id); p != kNoNode; p = g.parent(p))
                if (set.contains(p))
                    return true;
            return false;
        };
        const Turntable* lightsWith = nullptr;
        for (auto& [t, list] : lists)
        {
            if (!viewState::isViewport() && !t->as<bool>(Turntable::kIncludeInRenders))
                continue;
            if (wrap(t->angle) == 0.0 && t->pitch == 0.0)
                continue;
            const glm::dmat4 turn = liveTurn(g, *t);
            for (NodeId m : list)
                if (!underAny(m, onAny))
                    turns.emplace(m, turn);
            if (!lightsWith && t->as<bool>(Turntable::kLightsFollow))
                lightsWith = t;
        }
        if (lightsWith)
        {
            const glm::dmat4 turn = liveTurn(g, *lightsWith);
            for (NodeId id : g.nodes())
                if (const Node* n = g.node(id); n && n->type().isA("light") && !turns.contains(id) && !onAny.contains(id) && !underAny(id, onAny))
                    turns.emplace(id, turn);
        }
    }
    g.setDisplayTurns(std::move(turns));
}

void setPlaying(Graph& g, NodeId id, bool on)
{
    Turntable* t = get(g, id);
    if (!t)
        return;
    t->playing = on;
    t->held = false;
    t->coasting = false;
    if (on && t->seek)
        t->seek.reset();
}

void setAngle(Graph& g, NodeId id, double degrees)
{
    Turntable* t = get(g, id);
    if (!t)
        return;
    t->angle = wrap(degrees);
    t->velocity = 0.0;
    t->seek.reset();
    t->coasting = false;
    sync(g);
}

void turnTo(Graph& g, NodeId id, double degrees, double now, bool pauseAfter)
{
    Turntable* t = get(g, id);
    if (!t)
        return;
    const double d = delta(t->angle, degrees);
    // A seek from a spin starts where the spin is, so the hand-over keeps its speed roughly.
    const double duration = std::max(t->as<double>(Turntable::kEase), 0.15) * (0.6 + 0.4 * std::abs(d) / 180.0);
    t->seek = Turntable::Seek{t->angle, t->angle + d, now, duration, pauseAfter, t->pitch, t->pitch};
    t->velocity = 0.0;
    t->coasting = false;
}

void step(Graph& g, NodeId id, int n, double now)
{
    Turntable* t = get(g, id);
    if (!t || n == 0)
        return;
    const double stepAngle = std::clamp(t->as<double>(Turntable::kStepAngle), 1.0, 360.0);
    const double rest = t->as<double>(Turntable::kRestAngle);
    // From where a running seek is heading, so repeated presses queue up stops.
    const double from = t->seek ? t->seek->to : t->angle;
    const double k = (from - rest) / stepAngle;
    double stop = n > 0 ? std::floor(k + 1e-6) + n : std::ceil(k - 1e-6) + n;
    const double to = rest + stop * stepAngle;
    const double d = to - from;
    const double duration = std::max(t->as<double>(Turntable::kEase), 0.15) * (0.6 + 0.4 * std::min(std::abs(d), 180.0) / 180.0);
    t->seek = Turntable::Seek{t->angle, to, now, duration, true, t->pitch, t->pitch};
    t->playing = false;
    t->velocity = 0.0;
    t->coasting = false;
}

void front(Graph& g, NodeId id, double now)
{
    Turntable* t = get(g, id);
    if (!t)
        return;
    t->playing = false;
    turnTo(g, id, t->as<double>(Turntable::kRestAngle), now, true);
    // Long enough for the tilt to settle too, even when the angle is already home.
    t->seek->pitchTo = 0.0;
    t->seek->duration = std::max(t->seek->duration, std::max(t->as<double>(Turntable::kEase), 0.15) * (0.6 + 0.4 * std::abs(t->pitch) / 90.0));
}

void scaleSpeed(Graph& g, NodeId id, double factor)
{
    if (Turntable* t = get(g, id))
        t->speedScale = std::clamp(t->speedScale * factor, 0.05, 20.0);
}

void grab(Graph& g, NodeId id)
{
    if (Turntable* t = get(g, id))
    {
        t->dragging = true;
        t->velocity = 0.0;
        t->seek.reset();
        t->coasting = false;
    }
}

void drag(Graph& g, NodeId id, double degrees, double dt)
{
    Turntable* t = get(g, id);
    if (!t)
        return;
    t->angle = wrap(t->angle + degrees);
    if (dt > 1e-4)
    {
        // Smoothed so the release speed is the hand's, not the last mouse event's.
        const double k = 1.0 - std::exp(-dt / 0.05);
        t->velocity += (degrees / dt - t->velocity) * k;
    }
    sync(g);
}

void dragPitch(Graph& g, NodeId id, double degrees, const glm::dvec3& axis, const glm::dvec3& centre)
{
    Turntable* t = get(g, id);
    if (!t || glm::length(axis) < 1e-9)
        return;
    if (std::abs(t->pitch) < 1e-9)
    {
        t->pitchAxis = glm::normalize(axis);
        t->pitchCentre = centre;
    }
    t->pitch = std::clamp(t->pitch + degrees, -89.0, 89.0);
    sync(g);
}

void release(Graph& g, NodeId id)
{
    Turntable* t = get(g, id);
    if (!t)
        return;
    t->dragging = false;
    t->coasting = std::abs(t->velocity) > 1.0;
    if (!t->coasting)
        t->velocity = 0.0;
}

void memberEdited(Graph& g, NodeId id, double now)
{
    Turntable* t = get(g, id);
    if (!t || !t->as<bool>(Turntable::kHoldWhileEditing) || t->dragging)
        return;
    if (t->playing || t->coasting || t->held)
    {
        t->held = true;
        t->coasting = false;
        t->lastEdit = now;
    }
}

bool moving(const Turntable& t)
{
    if (t.dragging || t.seek || t.coasting || std::abs(t.velocity) > 1e-3)
        return true;
    return t.playing && !t.held;
}

bool advance(Graph& g, double dt, double now)
{
    dt = std::clamp(dt, 0.0, 0.1);
    bool any = false, changed = false;
    for (Turntable* t : all(g))
    {
        const double before = t->angle;
        const double pitchBefore = t->pitch;
        if (t->held && t->as<double>(Turntable::kResumeAfter) > 0.0 && now - t->lastEdit >= t->as<double>(Turntable::kResumeAfter))
            t->held = false;
        if (t->dragging)
        {
            // The hand holds it: the speed it is moved at decays when the hand stops.
            t->velocity *= std::exp(-dt / 0.08);
        }
        else if (t->seek)
        {
            const Turntable::Seek& s = *t->seek;
            const double x = (now - s.start) / std::max(s.duration, 1e-3);
            t->angle = wrap(s.from + (s.to - s.from) * smootherstep(x));
            t->pitch = s.pitchFrom + (s.pitchTo - s.pitchFrom) * smootherstep(x);
            if (x >= 1.0)
            {
                const bool pause = s.pauseAfter;
                t->seek.reset();
                t->velocity = 0.0;
                if (pause)
                    t->playing = false;
            }
        }
        else
        {
            const double cruise = t->playing && !t->held ? cruiseSpeed(*t) : 0.0;
            // Easing is an exponential approach of the speed, so it stays continuous through every change.
            double tau = std::max(t->as<double>(Turntable::kEase), 0.01) / 4.0;
            if (t->held)
                tau = 0.15 / 4.0;
            else if (t->coasting)
                tau = cruise == 0.0 ? 0.6 : std::max(tau * 2.0, 0.4);
            t->velocity = cruise + (t->velocity - cruise) * std::exp(-dt / tau);
            if (t->coasting && std::abs(t->velocity - cruise) < 0.5)
                t->coasting = false;
            if (cruise == 0.0 && std::abs(t->velocity) < 0.01)
                t->velocity = 0.0;
            t->angle = wrap(t->angle + t->velocity * dt);
        }
        changed = changed || t->angle != before || t->pitch != pitchBefore;
        any = any || moving(*t);
    }
    if (changed)
        sync(g);
    return any;
}

double environmentTurn(const Graph& g)
{
    if (viewState::isExport())
        return 0.0;
    for (Turntable* t : all(g))
        if (t->as<bool>(Turntable::kLightsFollow) && (viewState::isViewport() || t->as<bool>(Turntable::kIncludeInRenders)))
            return -t->angle;
    return 0.0;
}

double reach(const Graph& cg, NodeId turntable)
{
    Graph& g = const_cast<Graph&>(cg);
    if (!get(g, turntable))
        return 0.0;
    const glm::dvec3 p = pivot(g, turntable), a = axis(g, turntable);
    double r = 0.0;
    auto addBounds = [&](NodeId id) {
        const Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            return;
        const geo::Bounds b = worldBounds(g, id);
        if (!b.valid())
            return;
        for (int c = 0; c < 8; ++c)
        {
            const glm::dvec3 corner((c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z);
            const glm::dvec3 d = corner - p;
            r = std::max(r, glm::length(d - a * glm::dot(d, a)));
        }
    };
    for (NodeId m : members(g, turntable))
    {
        addBounds(m);
        for (NodeId d : g.descendants(m))
            addBounds(d);
    }
    return r;
}

std::optional<glm::dvec3> footprintCentre(const Graph& cg, const std::vector<NodeId>& nodes)
{
    Graph& g = const_cast<Graph&>(cg);
    geo::Bounds b;
    auto addBounds = [&](NodeId id) {
        if (const Node* n = g.node(id); n && n->type().isA("mesh"))
            if (geo::Bounds wb = worldBounds(g, id); wb.valid())
                b.extend(wb);
    };
    for (NodeId id : nodes)
    {
        addBounds(id);
        for (NodeId d : g.descendants(id))
            addBounds(d);
    }
    if (!b.valid())
    {
        for (NodeId id : nodes)
            if (const Node* n = g.node(id); n && n->type().dag)
                b.extend(glm::dvec3(n->as<glm::dmat4>(Transform::kWorldMatrix)[3]));
        if (!b.valid())
            return std::nullopt;
    }
    glm::dvec3 c = b.center();
    const glm::dvec3 up = sceneUp();
    // Drop the centre to the lowest point along up.
    c -= up * glm::dot(c - b.min, up);
    return c;
}

bool sweepBounds(const Graph& cg, const std::vector<NodeId>& framed, glm::dvec3& lo, glm::dvec3& hi)
{
    Graph& g = const_cast<Graph&>(cg);
    std::vector<NodeId> tables;
    for (NodeId id : framed)
    {
        if (Turntable* t = get(g, id))
        {
            add(tables, t->id());
            for (NodeId m : members(g, id))
                for (NodeId n : std::vector<NodeId>{m})
                {
                    auto extend = [&](NodeId x) {
                        if (const Node* xn = g.node(x); xn && xn->type().isA("mesh"))
                            if (geo::Bounds wb = worldBounds(g, x); wb.valid())
                            {
                                lo = glm::min(lo, wb.min);
                                hi = glm::max(hi, wb.max);
                            }
                    };
                    extend(n);
                    for (NodeId d : g.descendants(n))
                        extend(d);
                }
        }
        else if (NodeId on = turntableOf(g, id); on != kNoNode)
            add(tables, on);
    }
    if (tables.empty() || lo.x > hi.x)
        return false;
    glm::dvec3 outLo = lo, outHi = hi;
    for (NodeId t : tables)
    {
        const glm::dvec3 p = pivot(g, t), a = axis(g, t);
        double hMin = 1e300, hMax = -1e300, r = 0.0;
        for (int c = 0; c < 8; ++c)
        {
            const glm::dvec3 corner((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
            const double h = glm::dot(corner - p, a);
            hMin = std::min(hMin, h);
            hMax = std::max(hMax, h);
            r = std::max(r, glm::length(corner - p - a * h));
        }
        const glm::dvec3 ext(r * std::sqrt(std::max(0.0, 1.0 - a.x * a.x)), r * std::sqrt(std::max(0.0, 1.0 - a.y * a.y)),
                             r * std::sqrt(std::max(0.0, 1.0 - a.z * a.z)));
        for (const glm::dvec3& e : {p + a * hMin, p + a * hMax})
        {
            outLo = glm::min(outLo, e - ext);
            outHi = glm::max(outHi, e + ext);
        }
    }
    lo = outLo;
    hi = outHi;
    return true;
}

void registerTypes()
{
    if (types::find(kTypeName))
        return;
    data::registerKind(kMembersKind, &Members::fromJson);
    auto f = [](const char* name, double def, const char* doc, double lo, double hi) {
        return AttrDef{.name = name, .type = PlugType::Float, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc,
                       .minValue = lo, .softMax = hi};
    };
    auto b = [](const char* name, bool def, const char* doc) {
        return AttrDef{.name = name, .type = PlugType::Bool, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc};
    };
    NodeType t;
    t.name = kTypeName;
    t.icon = "rotate-360";
    t.base = types::find("transform");
    t.dag = true;
    t.doc = "Turns its members in front of a still camera, for streams and reviews: Alt+Shift+T spins it, Alt+Shift+drag turns it by "
            "hand. Its position is the pivot and its up axis the axis. The turn is view-only: exports and the Unreal link never see it.";
    AttrDef restAngle = f("restAngle", 0.0, "The angle Front returns to, degrees.", -360.0, 360.0);
    restAngle.minValue = -360.0;
    restAngle.maxValue = 360.0;
    t.ownAttrs = {
        f("secondsPerTurn", 20.0, "How long one full turn takes while it spins.", 1.0, 120.0),
        AttrDef{.name = "direction", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
                .enumNames = {"counterClockwise", "clockwise"}, .doc = "Which way it spins, seen from above."},
        f("ease", 0.8, "Seconds to reach full speed or come to rest; steps and Front take about this long too.", 0.0, 5.0),
        restAngle,
        f("stepAngle", 90.0, "The angle between stops for stepping (front, side, back, side at 90).", 1.0, 180.0),
        b("lightsFollow", false, "Turn the scene's lights and the environment with it: the light stays put on the model. Off, highlights travel across it."),
        b("holdWhileEditing", true, "Stop quickly while a member is edited, and spin on after resumeAfter seconds of quiet."),
        f("resumeAfter", 2.0, "Seconds of quiet after an edit before a held spin resumes; 0 waits for the next Spin.", 0.0, 30.0),
        b("includeInRenders", true, "Renders show the members turned as the viewport does."),
        AttrDef{.name = "members", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
                .doc = "The objects it turns, with everything under them."},
    };
    t.create = [] { return std::make_unique<Turntable>(); };
    types::registerType(std::move(t));

    Graph::addEvaluateHook([](Graph& g, bool after) {
        if (!after)
            sync(g);
    });
    viewState::onChange([] { sync(Graph::current()); });
}

void registerCommands()
{
    if (ops::find("turntable"))
        return;

    std::vector<FlagDef> attrFlags = {
        {"secondsPerTurn", "spt", T::Float, nullptr, "How long one full turn takes.", {}, {}, 1.0, 3600.0},
        {"direction", "dir", T::String, nullptr, "Spin direction seen from above.", {"counterClockwise", "clockwise"}},
        {"ease", "", T::Float, nullptr, "Seconds to reach speed or rest.", {}, {}, 0.0, 10.0},
        {"restAngle", "ra", T::Float, nullptr, "The angle Front returns to, degrees."},
        {"stepAngle", "sa", T::Float, nullptr, "Degrees between stops.", {}, {}, 1.0, 360.0},
        {"lightsFollow", "lf", T::Bool, nullptr, "Turn the scene's lights and environment with it."},
        {"holdWhileEditing", "hwe", T::Bool, nullptr, "Stop while a member is edited."},
        {"resumeAfter", "rsa", T::Float, nullptr, "Seconds of quiet before a held spin resumes; 0 never.", {}, {}, 0.0, 600.0},
        {"includeInRenders", "iir", T::Bool, nullptr, "Renders show the turn."},
        {"pivot", "pv", T::Vec3, nullptr, "World pivot; by default the centre of the members' footprint on the ground."},
    };

    std::vector<FlagDef> flags = attrFlags;
    flags.push_back({"name", "n", T::String, nullptr, "Name of a new turntable; made unique."});
    flags.push_back({"edit", "e", T::String, nullptr, "Change this turntable instead of making one."});
    flags.push_back({"query", "q", T::String, nullptr, "Return this turntable's members, live state and settings."});
    flags.push_back({"add", "a", T::List, nullptr, "With edit: objects to put on it."});
    flags.push_back({"remove", "rm", T::List, nullptr, "With edit: objects to take off it."});
    ops::registerCommand({
        .name = "turntable",
        .doc = "Make a turntable for objects (the selection by default), or change or query one. Members turn about the pivot "
               "while the camera stays still; the turn is view-only, so exports never see it. Returns the turntable's name, "
               "or with query its state.",
        .positionalDoc = "objects",
        .flags = flags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.has("query"))
            {
                Node& n = nodeArg(g, a.get<std::string>("query"), "turntable");
                Turntable* t = get(g, n.id());
                if (!t)
                    throw CommandError("turntable: " + n.path() + " is not a turntable");
                return query(g, *t);
            }
            if (a.has("edit"))
            {
                Node& n = nodeArg(g, a.get<std::string>("edit"), "turntable");
                Turntable* t = get(g, n.id());
                if (!t)
                    throw CommandError("turntable: " + n.path() + " is not a turntable");
                std::vector<NodeId> list = members(g, t->id());
                if (a.has("add"))
                    for (NodeId id : objectsArg(g, a.flags["add"], "turntable"))
                        add(list, id);
                if (a.has("remove"))
                    for (NodeId id : objectsArg(g, a.flags["remove"], "turntable"))
                        std::erase(list, id);
                if (list != members(g, t->id()))
                    setMembers(g, t->id(), list);
                applyFlags(g, *t, a);
                sync(g);
                return t->path();
            }
            std::vector<NodeId> list = a.positional.empty() ? selectedObjects(g) : objectsArg(g, a.positional, "turntable");
            if (list.empty())
                throw CommandError("turntable: select the objects to put on it");
            Turntable& t = create(g, list, a.get<std::string>("name", ""));
            applyFlags(g, t, a);
            sync(g);
            return t.path();
        },
        .examples = {"turntable(\"statue\", secondsPerTurn=30)", "turntable(edit=\"turntable1\", add=[\"plinth\"], lightsFollow=True)",
                     "turntable(query=\"turntable1\")"},
    });

    ops::registerCommand({
        .name = "putOnTurntable",
        .doc = "Put objects (the selection by default) on a turntable: the selected turntable, or the one other selected "
               "objects are on, else a new one under them. Returns the turntable's name.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> list = a.positional.empty() ? selectedObjects(g) : objectsArg(g, a.positional, "putOnTurntable");
            NodeId into = kNoNode;
            for (NodeId id : g.selection())
                if (get(g, id))
                {
                    into = id;
                    break;
                }
            std::vector<NodeId> fresh;
            for (NodeId id : list)
                if (NodeId on = turntableOf(g, id); on == kNoNode)
                    fresh.push_back(id);
                else if (into == kNoNode)
                    into = on;
            if (list.empty())
                throw CommandError("putOnTurntable: select the objects to put on it");
            UndoChunk chunk("Put on Turntable");
            if (into == kNoNode)
            {
                Turntable& t = create(g, fresh, "");
                sync(g);
                return t.path();
            }
            std::vector<NodeId> now = members(g, into);
            for (NodeId id : fresh)
                add(now, id);
            setMembers(g, into, now);
            lastUsed = into;
            sync(g);
            return g.node(into)->path();
        },
    });

    ops::registerCommand({
        .name = "takeOffTurntable",
        .doc = "Take objects (the selection by default) off their turntables; they go back to where they stand unturned. A "
               "turntable left empty is deleted. Returns the objects taken off.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> list = a.positional.empty() ? selectedObjects(g) : objectsArg(g, a.positional, "takeOffTurntable");
            // A selected turntable gives up all of its members.
            for (NodeId id : g.selection())
                if (get(g, id) && a.positional.empty())
                    for (NodeId m : members(g, id))
                        add(list, m);
            UndoChunk chunk("Take off Turntable");
            json out = json::array();
            for (Turntable* t : all(g))
            {
                std::vector<NodeId> now = members(g, t->id());
                const size_t before = now.size();
                for (NodeId id : list)
                    if (std::erase(now, id))
                        out.push_back(g.node(id)->path());
                if (now.size() == before)
                    continue;
                if (now.empty())
                    g.deleteNode(t->id());
                else
                    setMembers(g, t->id(), now);
            }
            sync(g);
            return out;
        },
    });

    ops::registerCommand({
        .name = "turntablePlay",
        .doc = "Spin a turntable (the selected one, the one the selection is on, else the last used), easing in; pause=True "
               "eases it to rest, toggle=True flips it. Not undoable, like a camera move. Returns whether it spins.",
        .positionalDoc = "turntable",
        .flags = {{"pause", "p", T::Bool, false, "Ease to rest instead."},
                  {"toggle", "tg", T::Bool, false, "Spin when resting, rest when spinning."},
                  {"speed", "sp", T::Float, nullptr, "Multiply the speed by this while it spins (1 is secondsPerTurn).", {}, {}, 0.05, 20.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Turntable& t = target(g, a, "turntablePlay");
            if (a.has("speed"))
                t.speedScale = std::clamp(a.get<double>("speed"), 0.05, 20.0);
            const bool on = a.get<bool>("toggle", false) ? !(t.playing && !t.held) : !a.get<bool>("pause", false);
            setPlaying(g, t.id(), on);
            return t.playing;
        },
        .examples = {"turntablePlay()", "turntablePlay(toggle=True)", "turntablePlay(\"turntable1\", pause=True)"},
    });

    ops::registerCommand({
        .name = "turntableTurn",
        .doc = "Turn a turntable (the selected one, the one the selection is on, else the last used): by= degrees, to= an "
               "angle, step= stops (stepAngle apart, negative goes back), or front=True back to restAngle. Eases unless "
               "ease=False. Not undoable. Returns the angle it is heading for.",
        .positionalDoc = "turntable",
        .flags = {{"by", "b", T::Float, nullptr, "Degrees to turn by."},
                  {"to", "t", T::Float, nullptr, "Angle to turn to, degrees."},
                  {"step", "st", T::Int, nullptr, "Stops to step, negative backwards."},
                  {"front", "f", T::Bool, false, "Back to restAngle with no tilt, and pause."},
                  {"pitch", "pi", T::Float, nullptr, "Tilt toward the camera (negative away), degrees, about the active view's horizontal "
                                                     "axis through the middle of what it turns.", {}, {}, -89.0, 89.0},
                  {"ease", "", T::Bool, true, "Ease there; False jumps."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Turntable& t = target(g, a, "turntableTurn");
            const bool ease = a.get<bool>("ease", true);
            if (a.has("pitch"))
            {
                const glm::dvec3 right(viewState::activeCamera().world[0]);
                glm::dvec3 lo(1e300), hi(-1e300);
                const glm::dvec3 centre = sweepBounds(g, {t.id()}, lo, hi) ? (lo + hi) * 0.5 : pivot(g, t.id());
                // The view's right axis tilts the top toward the camera for a positive angle.
                dragPitch(g, t.id(), a.get<double>("pitch") - t.pitch, glm::length(right) > 1e-9 ? right : glm::dvec3(1, 0, 0), centre);
                if (!a.has("by") && !a.has("to") && !a.has("step") && !a.get<bool>("front", false))
                    return wrap(t.angle);
            }
            double to = t.angle;
            if (a.get<bool>("front", false))
            {
                to = t.as<double>(Turntable::kRestAngle);
                t.playing = false;
            }
            else if (a.has("step"))
            {
                step(g, t.id(), a.get<int>("step"), now());
                if (!ease)
                    setAngle(g, t.id(), t.seek ? t.seek->to : t.angle);
                return wrap(t.seek ? t.seek->to : t.angle);
            }
            else if (a.has("to"))
                to = a.get<double>("to");
            else if (a.has("by"))
                to = t.angle + a.get<double>("by");
            else
                throw CommandError("turntableTurn: give by=, to=, step=, pitch= or front=True");
            if (ease)
                turnTo(g, t.id(), to, now(), a.get<bool>("front", false));
            else
                setAngle(g, t.id(), to);
            return wrap(to);
        },
        .examples = {"turntableTurn(step=1)", "turntableTurn(to=180)", "turntableTurn(front=True)"},
    });

    ops::registerCommand({
        .name = "turntableBake",
        .doc = "Bake a turntable into real animation for export: a new transform at the pivot keyed through turns= full "
               "turns from start= to end= (frames, the playback range by default), with the members parented under it. "
               "The members leave the turntable. Returns the new transform.",
        .positionalDoc = "turntable",
        .flags = {{"start", "s", T::Float, nullptr, "First frame; the playback range's start by default."},
                  {"end", "en", T::Float, nullptr, "Last frame; the playback range's end by default."},
                  {"turns", "tu", T::Float, 1.0, "Full turns over the range.", {}, {}, 0.01, 1000.0},
                  {"name", "n", T::String, "turntableSpin", "Name of the new transform; made unique."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Turntable& t = target(g, a, "turntableBake");
            const std::vector<NodeId> list = members(g, t.id());
            if (list.empty())
                throw CommandError("turntableBake: the turntable has no members");
            const double fps = ops::execute("playbackOptions", {{"query", true}}).value("fps", 30.0);
            json range = ops::execute("playbackOptions", {{"query", true}});
            const double start = a.has("start") ? a.get<double>("start") : range.value("minTime", 0.0) * fps;
            const double end = a.has("end") ? a.get<double>("end") : range.value("maxTime", 4.0) * fps;
            if (end <= start)
                throw CommandError("turntableBake: end must be after start");
            UndoChunk chunk("Bake Turntable");
            // Unturned, so the members keep the placement they have without the turntable.
            setAngle(g, t.id(), 0.0);
            t.playing = false;
            g.evaluate();
            Node* spin = g.createNode(*types::find("transform"), a.get<std::string>("name", "turntableSpin"));
            const glm::dmat4 frame = frameOf(g, t);
            Transform::setLocalMatrix(g, spin->id(), frame);
            g.evaluate();
            for (NodeId m : list)
                g.setParent(m, spin->id(), true);
            const int upAxis = std::abs(sceneUp().z) > 0.5 ? 2 : 1;
            const char* channel = upAxis == 2 ? "rz" : "ry";
            const double r0 = spin->as<glm::dvec3>(Transform::kRotate)[upAxis];
            const double sign = t.as<int64_t>(Turntable::kDirection) == 1 ? -1.0 : 1.0;
            const std::string plug = spin->path() + "." + channel;
            ops::execute("setKeyframe", {{"args", {plug}}, {"time", start / fps}, {"value", r0},
                                         {"inTangentType", "linear"}, {"outTangentType", "linear"}});
            ops::execute("setKeyframe", {{"args", {plug}}, {"time", end / fps}, {"value", r0 + sign * 360.0 * a.get<double>("turns", 1.0)},
                                         {"inTangentType", "linear"}, {"outTangentType", "linear"}});
            g.deleteNode(t.id());
            sync(g);
            return spin->path();
        },
    });
}
} // namespace lila::scene::turntable
