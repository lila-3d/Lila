#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "core/Profile.h"
#include "geo/Conform.h"
#include "geo/LoopTools.h"
#include "geo/Mesh.h"
#include "geo/Topology.h"
#include "scene/Components.h"
#include "scene/ConformTargets.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/SphereState.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <map>
#include <memory>

namespace lila::scene
{
namespace
{
using components::Ids;
using geo::Index;

using MovesFn = std::function<geo::PointMoves(const geo::Mesh&, const glm::dmat4& world, const geo::ShapeSelection&)>;

geo::Spacing spacingArg(const Args& a, const char* fallback)
{
    const std::string s = a.get<std::string>("spacing", fallback);
    return s == "keep" ? geo::Spacing::Keep : s == "project" ? geo::Spacing::Project : geo::Spacing::Even;
}

/** A world distance in the mesh's object space, by its average scale. */
double toLocalLength(const glm::dmat4& world, double cm)
{
    const double scale = std::cbrt(std::abs(glm::determinant(glm::dmat3(world))));
    return scale > 1e-12 ? cm / scale : cm;
}

geo::ShapeSelection shapeOf(const geo::Mesh& m, ComponentMode kind, const Ids& ids)
{
    switch (kind)
    {
    case ComponentMode::Edge: return geo::shapeOfEdges(m, ids);
    case ComponentMode::Face: return geo::shapeOfFaces(m, ids);
    default: return geo::shapeOfPoints(m, ids);
    }
}

/** Moved points go strength of the way to their target; soft-selected ones take the displacement of the nearest moved point. */
PointTarget targetOfMoves(const geo::Mesh& m, const geo::PointMoves& moves, double strength)
{
    struct Data
    {
        std::vector<glm::dvec3> offset;
        std::vector<uint8_t> has;
        std::vector<glm::dvec3> restAt;
        std::vector<glm::dvec3> offsetAt;
    };
    auto d = std::make_shared<Data>();
    d->offset.assign(m.pointCapacity(), glm::dvec3(0.0));
    d->has.assign(m.pointCapacity(), 0);
    for (const geo::PointMove& mv : moves)
    {
        const glm::dvec3 off = (mv.position - m.position(mv.point)) * strength;
        d->offset[mv.point] = off;
        d->has[mv.point] = 1;
        d->restAt.push_back(m.position(mv.point));
        d->offsetAt.push_back(off);
    }
    return [d](uint32_t p, const glm::dvec3& rest) {
        if (p < d->has.size() && d->has[p])
            return rest + d->offset[p];
        size_t best = 0;
        double bestD = std::numeric_limits<double>::max();
        for (size_t i = 0; i < d->restAt.size(); ++i)
            if (double dist = glm::dot(d->restAt[i] - rest, d->restAt[i] - rest); dist < bestD)
                bestD = dist, best = i;
        return d->restAt.empty() ? rest : rest + d->offsetAt[best];
    };
}

/** Runs fn on each mesh's selection seen as chains; the selection's face interiors follow its borders. */
void conformSelected(const char* command, const char* what, double strength, const MovesFn& fn)
{
    LILA_ZONE();

    int edited = deformSelectedComponents(command, [&](const geo::Mesh& m, const glm::dmat4& world, ComponentMode kind, const Ids& ids) -> PointTarget {
        geo::ShapeSelection sel = shapeOf(m, kind, ids);
        geo::PointMoves moves = fn(m, world, sel);
        if (moves.empty())
            return nullptr;
        geo::followInterior(m, sel.interior, moves);
        return targetOfMoves(m, moves, strength);
    });
    if (edited == 0)
        throw CommandError(std::format("{}: {}", command, what));
}

SphereState& sphereState()
{
    static SphereState state;
    return state;
}

std::map<std::string, conformTargets::Provider>& providers()
{
    static std::map<std::string, conformTargets::Provider> p;
    return p;
}

bool conformable(Node& n) { return n.type().isA("mesh") || providers().contains(n.type().name); }

/** The target= mesh or curve, else the one selected mesh or curve with no components selected in the current mode. */
Node& conformTarget(const Args& a)
{
    Graph& g = Graph::current();
    if (std::string name = a.get<std::string>("target", ""); !name.empty())
    {
        Node* n = g.find(name);
        if (!n || !conformable(*n))
            throw CommandError("polyConformToObject: " + name + " is not a mesh or curve");
        return *n;
    }
    Node* found = nullptr;
    const auto& selected = components::selected(components::mode());
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && conformable(*n) && !selected.contains(id))
            found = n;
    if (!found)
        throw CommandError("polyConformToObject: give target= or also select the mesh or curve to conform to");
    return *found;
}

geo::PointMoves shapeMoves(const std::string& shape, const Args& a, const geo::Mesh& m, const glm::dmat4& world, const geo::ShapeSelection& sel)
{
    const double radius = toLocalLength(world, std::max(0.0, a.get<double>("radius", 0.0)));
    const geo::Spacing spacing = spacingArg(a, "even");
    const std::vector<Index> all = geo::shapePoints(sel);
    if (shape == "line")
        return sel.chains.empty() ? geo::alignToLine(m, all) : geo::straighten(m, sel, {spacing, true});
    if (shape == "square" || shape == "rectangle")
        return geo::rectangularize(m, sel, {spacing, shape == "square", a.get<double>("rotation", 0.0), true});
    if (shape == "sphere")
        return geo::spherize(m, all, {.radius = radius});
    if (shape == "cylinder")
    {
        const std::string axis = a.get<std::string>("axis", "auto");
        return geo::cylinderize(m, all, axis == "x" ? 0 : axis == "y" ? 1 : axis == "z" ? 2 : -1, radius);
    }
    if (shape == "plane")
        return geo::flattenToPlane(m, all);
    geo::CircleOptions o{spacing, radius, a.get<double>("rotation", 0.0), a.get<int>("sides", 0), true};
    return geo::circularize(m, sel, o);
}

void registerOptionLayouts()
{
    namespace opt = ops::options;
    using O = opt::Override;
    const O::Range strength{0.0, 1.0, 0.05, ""}, radius{0.0, 1e6, 0.1, " cm"}, rotation{-360.0, 360.0, 1.0, " deg"}, sides{0.0, 64.0, 1.0, ""};
    const std::vector<std::pair<std::string, std::string>> spacing = {{"even", "Even"}, {"keep", "Keep Relative"}, {"project", "Nearest Point"}};

    O straighten;
    straighten.choices["spacing"] = spacing;
    straighten.ranges["strength"] = strength;
    straighten.labels = {{"fitLine", "Best-Fit Line"}};
    opt::setOverride("polyStraighten", std::move(straighten));

    O circle;
    circle.order = {"spacing", "radius", "rotation", "sides", "flatten", "strength"};
    circle.choices["spacing"] = spacing;
    circle.ranges = {{"strength", strength}, {"radius", radius}, {"rotation", rotation}, {"sides", sides}};
    circle.labels = {{"radius", "Radius (0 = Fit)"}, {"sides", "Polygon Sides (0 = Circle)"}, {"flatten", "Flatten to Plane"}};
    opt::setOverride("polyCircularize", std::move(circle));

    O sphere;
    sphere.order = {"amount", "fit", "radiusMode", "radius", "scale", "rotation", "alongNormals"};
    sphere.hidden = {"center"};
    sphere.adjust = {"scale", "rotation"};
    sphere.choices["fit"] = {{"best", "Best-Fit Sphere"}, {"center", "Around the Centroid"}};
    sphere.choices["radiusMode"] = {{"average", "Average Distance"}, {"fit", "Fitted Radius"}, {"min", "Nearest Point"}, {"max", "Farthest Point"}, {"explicit", "Radius Below"}};
    sphere.ranges = {{"amount", strength}, {"radius", radius}};
    sphere.labels = {{"radius", "Radius (0 = From Mode)"}, {"alongNormals", "Move Along Normals"}, {"scale", "Ellipsoid Scale"}, {"rotation", "Ellipsoid Rotation"}};
    opt::setOverride("polySpherize", std::move(sphere));

    O shape;
    shape.order = {"shape", "spacing", "radius", "rotation", "sides", "axis", "strength"};
    shape.choices["shape"] = {{"line", "Line"},         {"circle", "Circle"},     {"square", "Square"}, {"rectangle", "Rectangle"},
                              {"sphere", "Sphere"},     {"cylinder", "Cylinder"}, {"plane", "Plane"}};
    shape.choices["spacing"] = spacing;
    shape.choices["axis"] = {{"auto", "Best Fit"}, {"x", "X"}, {"y", "Y"}, {"z", "Z"}};
    shape.ranges = {{"strength", strength}, {"radius", radius}, {"rotation", rotation}, {"sides", sides}};
    shape.labels = {{"radius", "Radius (0 = Fit)"}, {"sides", "Polygon Sides (0 = Circle)"}, {"axis", "Cylinder Axis"}};
    opt::setOverride("polyConformShape", std::move(shape));

    O object;
    object.hidden = {"target"};
    object.choices["mode"] = {{"loop", "Nearest Edge Loop"}, {"surface", "Nearest Surface Point"}, {"vertex", "Nearest Vertex"}};
    object.choices["spacing"] = spacing;
    object.ranges["strength"] = strength;
    opt::setOverride("polyConformToObject", std::move(object));

    O flow;
    flow.ranges = {{"adjustEdgeFlow", {-2.0, 3.0, 0.05, ""}}, {"tension", {-1.0, 1.0, 0.05, ""}}, {"loopSmooth", strength}, {"iterations", {1.0, 20.0, 1.0, ""}}};
    flow.labels = {{"adjustEdgeFlow", "Amount"}, {"edgeFlow", "Curvature from Next Vertices"}, {"loopSmooth", "Smooth Along Loop"}};
    opt::setOverride("polyEditEdgeFlow", std::move(flow));
}
} // namespace

const SphereState& lastSphere() { return sphereState(); }

namespace conformTargets
{
void add(std::string nodeType, Provider p) { providers()[std::move(nodeType)] = std::move(p); }

std::optional<geo::Mesh> worldGeometry(Graph& g, Node& n)
{
    auto it = providers().find(n.type().name);
    return it == providers().end() ? std::nullopt : it->second(g, n);
}
} // namespace conformTargets

void registerConformCommands()
{
    using T = ArgType;
    const FlagDef strength{"strength", "", T::Float, 1.0, "How far toward the shape, 0 to 1; soft selection fades it further."};
    const std::vector<std::string> spacings = {"even", "keep", "project"};

    ops::registerCommand({
        .name = "polyStraighten",
        .doc = "Straighten the selected edge chains (or chains of selected vertices) into lines between their ends. Several separate "
               "chains straighten separately; closed loops are left alone. Vertices with no edges between them line up on their best-fit line.",
        .flags = {{"spacing", "", T::String, "keep", "keep (relative distances along the chain), even or project (straight across).", spacings},
                  {"fitLine", "", T::Bool, false, "Use the line through all of a chain's points instead of its ends."},
                  strength},
        .run = [](const Args& a) -> json {
            geo::StraightenOptions o{spacingArg(a, "keep"), a.get<bool>("fitLine")};
            conformSelected("polyStraighten", "select open edge chains or vertices to straighten", a.get<double>("strength", 1.0),
                            [&](const geo::Mesh& m, const glm::dmat4&, const geo::ShapeSelection& sel) { return geo::straighten(m, sel, o); });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "polyCircularize",
        .doc = "Fit the selected edge loops, the border of each selected face region, or the selected vertices to a circle in their "
               "best-fit plane, turned to move the points least. Points inside a face region follow the border smoothly, so a "
               "rectangular face comes out round, ready to extrude into a pipe. Open chains become an arc between their ends.",
        .flags = {{"spacing", "", T::String, "even", "even (equal angles), keep (relative distances along the loop) or project (straight out to the circle).", spacings},
                  {"radius", "", T::Float, 0.0, "Radius, cm; 0 keeps the loop's average radius."},
                  {"rotation", "", T::Float, 0.0, "Extra turn about the circle's axis, degrees."},
                  {"sides", "", T::Int, 0, "3 or more: conform to a regular polygon with this many sides, its corners on the circle."},
                  {"flatten", "", T::Bool, true, "Put the points in the circle's plane; off keeps each one's height off it."},
                  strength},
        .run = [](const Args& a) -> json {
            conformSelected("polyCircularize", "select edge loops, faces or vertices to make round", a.get<double>("strength", 1.0),
                            [&](const geo::Mesh& m, const glm::dmat4& world, const geo::ShapeSelection& sel) {
                                geo::CircleOptions o{spacingArg(a, "even"), toLocalLength(world, std::max(0.0, a.get<double>("radius"))),
                                                     a.get<double>("rotation"), a.get<int>("sides"), a.get<bool>("flatten", true)};
                                return geo::circularize(m, sel, o);
                            });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "polySpherize",
        .doc = "Push the selected components onto a sphere fitted to them, or onto an ellipsoid with scale. center and radius override "
               "the fit (world space, cm); radiusMode picks the radius from the points otherwise. With alongNormals each point slides along "
               "its own normal to the sphere. Returns the sphere used: {center, radius, scale, rotation}, world space.",
        .flags = {{"amount", "", T::Float, 1.0, "How far toward the sphere, 0 to 1; soft selection fades it further."},
                  {"fit", "", T::String, "best", "best (least-squares sphere) or center (around the points' centroid).", {"best", "center"}},
                  {"radiusMode", "", T::String, "average", "Radius when radius is 0: average distance, fit (the fitted radius), min, max or explicit.",
                   {"average", "fit", "min", "max", "explicit"}},
                  {"radius", "", T::Float, 0.0, "Radius, cm; above 0 it replaces radiusMode."},
                  {"center", "", T::Vec3, nullptr, "Sphere centre, world space cm; default: the fitted centre."},
                  {"scale", "", T::Vec3, json::array({1.0, 1.0, 1.0}), "Ellipsoid semi-axis factors along its X, Y and Z."},
                  {"rotation", "", T::Vec3, json::array({0.0, 0.0, 0.0}), "Turn of the ellipsoid's axes in world space, degrees (XYZ)."},
                  {"alongNormals", "", T::Bool, false, "Move each point along its own normal onto the sphere."}},
        .run = [](const Args& a) -> json {
            const auto vec = [&](const char* flag, const glm::dvec3& fallback) {
                const json j = a.flags.contains(flag) ? a.flags.at(flag) : json();
                return j.is_array() && j.size() == 3 ? glm::dvec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()) : fallback;
            };
            const glm::dvec3 scale = vec("scale", glm::dvec3(1.0)), rotation = vec("rotation", glm::dvec3(0.0));
            const glm::dquat turn = glm::angleAxis(glm::radians(rotation.z), glm::dvec3(0, 0, 1)) * glm::angleAxis(glm::radians(rotation.y), glm::dvec3(0, 1, 0)) *
                                    glm::angleAxis(glm::radians(rotation.x), glm::dvec3(1, 0, 0));
            const std::string mode = a.get<std::string>("radiusMode", "average");
            SphereState state;
            bool any = false;
            conformSelected("polySpherize", "no components selected", 1.0,
                            [&](const geo::Mesh& m, const glm::dmat4& world, const geo::ShapeSelection& sel) {
                                geo::SpherizeOptions o;
                                o.fit = a.get<std::string>("fit", "best") == "center" ? geo::SphereFit::Center : geo::SphereFit::Best;
                                o.radiusMode = mode == "fit"        ? geo::SphereRadius::Fit
                                               : mode == "min"      ? geo::SphereRadius::Min
                                               : mode == "max"      ? geo::SphereRadius::Max
                                               : mode == "explicit" ? geo::SphereRadius::Explicit
                                                                    : geo::SphereRadius::Average;
                                o.amount = std::clamp(a.get<double>("amount", 1.0), 0.0, 1.0);
                                o.radius = toLocalLength(world, std::max(0.0, a.get<double>("radius", 0.0)));
                                if (a.has("center"))
                                    o.center = glm::dvec3(glm::inverse(world) * glm::dvec4(vec("center", glm::dvec3(0.0)), 1.0));
                                o.scale = scale;
                                glm::dmat3 linear(world);
                                for (int i = 0; i < 3; ++i)
                                    linear[i] = glm::length(linear[i]) > 1e-12 ? glm::normalize(linear[i]) : glm::dvec3(i == 0, i == 1, i == 2);
                                o.frame = glm::transpose(linear) * glm::mat3_cast(turn);
                                o.alongNormals = a.get<bool>("alongNormals", false);
                                geo::SpherizeResult fitted;
                                geo::PointMoves moves = geo::spherize(m, geo::shapePoints(sel), o, &fitted);
                                if (!moves.empty())
                                {
                                    const double local = toLocalLength(world, 1.0);
                                    state.center = glm::dvec3(world * glm::dvec4(fitted.center, 1.0));
                                    state.radius = local > 1e-12 ? fitted.radius / local : fitted.radius;
                                    any = true;
                                }
                                return moves;
                            });
            if (any)
            {
                state.scale = scale;
                state.rotation = rotation;
                state.serial = sphereState().serial + 1;
                sphereState() = state;
            }
            return json{{"center", {state.center.x, state.center.y, state.center.z}}, {"radius", state.radius}, {"scale", {scale.x, scale.y, scale.z}},
                        {"rotation", {rotation.x, rotation.y, rotation.z}}};
        },
    });

    ops::registerCommand({
        .name = "polyConformShape",
        .doc = "Conform the selected components to a fitted shape: line, circle, square or rectangle (per chain or loop, corners "
               "on the points nearest the fitted rectangle's), sphere, cylinder (fitted axis) or plane (flatten). Face interiors "
               "follow their borders for the loop shapes.",
        .flags = {{"shape", "", T::String, "circle", "line, circle, square, rectangle, sphere, cylinder or plane.",
                   {"line", "circle", "square", "rectangle", "sphere", "cylinder", "plane"}},
                  {"spacing", "", T::String, "even", "Line, circle, square, rectangle: even, keep or project.", spacings},
                  {"radius", "", T::Float, 0.0, "Circle, sphere, cylinder: radius, cm; 0 fits it."},
                  {"rotation", "", T::Float, 0.0, "Circle, square, rectangle: extra turn in the plane, degrees."},
                  {"sides", "", T::Int, 0, "Circle: 3 or more makes it a regular polygon."},
                  {"axis", "", T::String, "auto", "Cylinder: auto (fitted), x, y or z (object axes).", {"auto", "x", "y", "z"}},
                  strength},
        .run = [](const Args& a) -> json {
            const std::string shape = a.get<std::string>("shape", "circle");
            conformSelected("polyConformShape", "the selection has nothing that shape fits", a.get<double>("strength", 1.0),
                            [&](const geo::Mesh& m, const glm::dmat4& world, const geo::ShapeSelection& sel) { return shapeMoves(shape, a, m, world, sel); });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "polyConformToObject",
        .doc = "Conform the selected components onto another mesh or a curve. loop: each selected edge loop (or face region border) lands "
               "one to one on the target's nearest closed loop (a cylinder's rim, a ring), at the turn that moves it least, or "
               "spread along it when the counts differ; face interiors follow. surface: every point to the nearest surface point. "
               "vertex: every point to the nearest target vertex. A curve target works through its line: loop spreads a loop along a "
               "closed curve, surface puts each point on the nearest point of the curve.",
        .flags = {{"target", "", T::String, nullptr, "Mesh or curve to conform to; default: the selected mesh or curve that has no components selected."},
                  {"mode", "", T::String, "loop", "loop, surface or vertex.", {"loop", "surface", "vertex"}},
                  {"spacing", "", T::String, "even", "Loop mode with unequal counts: even, keep or project.", spacings},
                  strength},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            Node& t = conformTarget(a);
            MeshPtr target;
            glm::dmat4 targetWorld(1.0);
            if (t.type().isA("mesh"))
            {
                target = t.as<MeshPtr>(MeshNode::kOutMesh);
                targetWorld = t.as<glm::dmat4>(Transform::kWorldMatrix);
            }
            else if (std::optional<geo::Mesh> w = conformTargets::worldGeometry(g, t))
                target = std::make_shared<geo::Mesh>(std::move(*w));
            if (!target || target->numPoints() == 0)
                throw CommandError("polyConformToObject: " + t.path() + " has no geometry");
            const std::string mode = a.get<std::string>("mode", "loop");
            geo::ConformOptions o{mode == "surface" ? geo::ConformMode::Surface : mode == "vertex" ? geo::ConformMode::Vertex : geo::ConformMode::Loop,
                                  spacingArg(a, "even")};
            if (components::selected(components::mode()).contains(t.id()))
                throw CommandError("polyConformToObject: " + t.path() + " cannot conform to itself; deselect its components");
            int edited = deformSelectedComponents("polyConformToObject", [&](const geo::Mesh& m, const glm::dmat4& world, ComponentMode kind,
                                                                             const Ids& ids) -> PointTarget {
                geo::Mesh local = *target;
                const glm::dmat4 toLocal = glm::inverse(world) * targetWorld;
                for (Index p = 0; p < local.pointCapacity(); ++p)
                    if (local.pointValid(p))
                        local.setPosition(p, glm::dvec3(toLocal * glm::dvec4(local.position(p), 1.0)));
                geo::ShapeSelection sel = shapeOf(m, kind, ids);
                geo::PointMoves moves = geo::conformToMesh(m, sel, local, o);
                if (moves.empty())
                    return nullptr;
                if (o.mode == geo::ConformMode::Loop)
                    geo::followInterior(m, sel.interior, moves);
                return targetOfMoves(m, moves, a.get<double>("strength", 1.0));
            });
            if (edited == 0)
                throw CommandError("polyConformToObject: nothing to conform; select components on another mesh");
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "polyEditEdgeFlow",
        .doc = "Move the selected edge loops onto the curvature of the surface around them (Maya's Edit Edge Flow). Each vertex lands "
               "on a curve through the nearest unselected vertices across the loop, shaped by the vertices beyond them, and neighbouring "
               "selected loops spread evenly between those, as subdividing would place them. Selected vertices off any chain, and "
               "face selections, fit both directions across each vertex.",
        .flags = {{"adjustEdgeFlow", "", T::Float, 1.0, "0 flat between the neighbours, 1 on the curve, above 1 exaggerated, negative inverted."},
                  {"edgeFlow", "", T::Bool, true, "Shape the curve from the vertices beyond the neighbours; off, from the surface normals at the neighbours."},
                  {"tension", "", T::Float, 0.0, "Toward 1 flattens the curve, negative makes it fuller."},
                  {"loopSmooth", "", T::Float, 0.0, "Also smooth each loop along its length, 0 to 1, keeping its spacing."},
                  {"iterations", "", T::Int, 1, "Passes of Smooth Along Loop."}},
        .run = [](const Args& a) -> json {
            geo::EdgeFlowOptions o{a.get<double>("adjustEdgeFlow"), a.get<bool>("edgeFlow"), a.get<double>("tension"), a.get<double>("loopSmooth"),
                                   std::clamp(a.get<int>("iterations"), 1, 100)};
            int edited = deformSelectedComponents("polyEditEdgeFlow", [&](const geo::Mesh& m, const glm::dmat4&, ComponentMode kind, const Ids& ids) -> PointTarget {
                std::vector<Index> edges, loose;
                if (kind == ComponentMode::Edge)
                    edges = ids;
                else
                    geo::flowSelectionOfPoints(m, kind == ComponentMode::Face ? geo::pointsOfFaces(m, ids) : ids, edges, loose);
                geo::PointMoves moves = geo::editEdgeFlow(m, edges, loose, o);
                return moves.empty() ? nullptr : targetOfMoves(m, moves, 1.0);
            });
            if (edited == 0)
                throw CommandError("polyEditEdgeFlow: select edge loops, vertices or faces to adjust");
            return nullptr;
        },
    });

    for (const char* name : {"polyStraighten", "polyCircularize", "polySpherize", "polyConformShape", "polyConformToObject", "polyEditEdgeFlow"})
        ops::setAdjustable(name);
    registerOptionLayouts();
}
} // namespace lila::scene
