#include "gn/NodesCommon.h"

namespace lila::gn::nodes
{
namespace
{
void groupNodes()
{
    registerDecl({
        .type = "GroupInput",
        .label = "Group Input",
        .category = "Group",
        .doc = "The modifier's input mesh and the group's exposed parameters.",
        .dynamicSockets =
            [](const Group& g, const GNode&) {
                Sockets s;
                s.outputs.push_back(geoOut("geometry", "The mesh coming up the modifier stack."));
                for (const GInput& in : g.inputs)
                    s.outputs.push_back(out(in.name, in.type, in.description.empty() ? "Exposed parameter." : in.description));
                return s;
            },
        .exec =
            [](ExecContext& c) {
                c.out("geometry", c.env.input ? c.env.input : makeGeometry({}));
                for (const GInput& in : c.group.inputs)
                {
                    int k = c.sockets.outputIndex(in.name);
                    if (k < 0)
                        continue;
                    auto it = c.env.params.find(in.name);
                    c.outputs[k] = it != c.env.params.end() ? it->second : paramValue(in, in.defaultValue);
                }
            },
    });
    registerDecl({
        .type = "GroupOutput",
        .label = "Group Output",
        .category = "Group",
        .doc = "What the modifier passes up the stack. Instances are realized and only the mesh is kept. The group's outputs are what "
               "a Group node using it gives back.",
        .dynamicSockets =
            [](const Group& g, const GNode&) {
                Sockets s;
                s.inputs.push_back(geoIn("geometry"));
                for (const GOutput& o : g.outputs)
                    s.inputs.push_back(fin(o.name, o.type, zeroJson(o.type), Implicit::None, o.description));
                return s;
            },
    });
}

void fieldInput(const char* type, const char* label, SocketType t, FieldPtr (*make)(), const char* doc)
{
    registerDecl({
        .type = type,
        .label = label,
        .category = "Input",
        .doc = doc,
        .sockets = {.outputs = {fout("value", t)}},
        .exec = [make](ExecContext& c) { c.out("value", make()); },
    });
}

void constantInput(const char* type, const char* label, SocketType t, json def)
{
    registerDecl({
        .type = type,
        .label = label,
        .category = "Input",
        .doc = std::string("A constant ") + socketTypeName(t) + ".",
        .sockets = {.inputs = {in("value", t, def)}, .outputs = {out("value", t)}},
        .exec =
            [t](ExecContext& c) {
                if (t == SocketType::String)
                    c.outString("value", c.s("value"));
                else
                    c.out("value", c.field("value"));
            },
    });
}

Sockets randomSockets(const Group&, const GNode& n)
{
    const NodeDecl* d = findDecl("RandomValue");
    SocketType t = dataTypeOf(d->propValue(n, "dataType"));
    Sockets s;
    switch (t)
    {
    case SocketType::Vector:
    case SocketType::Rotation:
        s.inputs = {fin("min", t, vec(0, 0, 0)), fin("max", t, t == SocketType::Rotation ? vec(360, 360, 360) : vec(1, 1, 1))};
        break;
    case SocketType::Int: s.inputs = {fin("min", t, 0), fin("max", t, 100)}; break;
    case SocketType::Bool: s.inputs = {fin("probability", SocketType::Float, 0.5)}; break;
    default: s.inputs = {fin("min", t, 0.0), fin("max", t, 1.0)}; break;
    }
    s.inputs.push_back(fin("id", SocketType::Int, nullptr, Implicit::Id, "Stable per-element key; defaults to the ID."));
    s.inputs.push_back(fin("seed", SocketType::Int, 0));
    s.outputs = {fout("value", t)};
    return s;
}

void randomValue()
{
    registerDecl({
        .type = "RandomValue",
        .label = "Random Value",
        .category = "Utilities",
        .doc = "Random numbers that stay the same for an element as long as its ID and the seed do.",
        .props = {prop("dataType", {"float", "int", "vector", "bool", "rotation"}, "float")},
        .dynamicSockets = randomSockets,
        .exec =
            [](ExecContext& c) {
                SocketType t = dataTypeOf(c.prop("dataType"));
                FieldPtr id = c.field("id"), seed = c.field("seed");
                auto u = [](int64_t i, int64_t s, uint32_t k) { return hashToUnit(hash32(uint32_t(i), uint32_t(s), k)); };
                switch (t)
                {
                case SocketType::Bool:
                    c.out("value", map<uint8_t, double, int64_t, int64_t>(
                                       T::Bool, [u](double p, int64_t i, int64_t s) { return u(i, s, 7) < p; },
                                       {c.field("probability"), id, seed}));
                    break;
                case SocketType::Int:
                    c.out("value", map<int64_t, int64_t, int64_t, int64_t, int64_t>(
                                       T::Int,
                                       [u](int64_t lo, int64_t hi, int64_t i, int64_t s) {
                                           if (hi < lo)
                                               std::swap(lo, hi);
                                           return lo + std::min<int64_t>(hi - lo, int64_t(u(i, s, 3) * double(hi - lo + 1)));
                                       },
                                       {c.field("min"), c.field("max"), id, seed}));
                    break;
                case SocketType::Vector:
                case SocketType::Rotation:
                    c.out("value", map<glm::dvec3, glm::dvec3, glm::dvec3, int64_t, int64_t>(
                                       t,
                                       [u](glm::dvec3 lo, glm::dvec3 hi, int64_t i, int64_t s) {
                                           return lo + (hi - lo) * glm::dvec3(u(i, s, 11), u(i, s, 12), u(i, s, 13));
                                       },
                                       {c.field("min"), c.field("max"), id, seed}));
                    break;
                default:
                    c.out("value", map<double, double, double, int64_t, int64_t>(
                                       T::Float, [u](double lo, double hi, int64_t i, int64_t s) { return lo + (hi - lo) * u(i, s, 1); },
                                       {c.field("min"), c.field("max"), id, seed}));
                    break;
                }
            },
    });
}

void sceneTime()
{
    registerDecl({
        .type = "SceneTime",
        .label = "Scene Time",
        .category = "Input",
        .doc = "The current time. A group using this makes its modifier re-evaluate whenever the time changes.",
        .sockets = {.inputs = {in("fps", T::Float, 30.0, "Frames per second for the frame output.")},
                    .outputs = {out("seconds", T::Float), out("frame", T::Float)}},
        .exec =
            [](ExecContext& c) {
                c.out("seconds", constantFloat(c.env.time));
                c.out("frame", constantFloat(c.env.time * c.f("fps")));
            },
    });
}

void objectInfo()
{
    registerDecl({
        .type = "ObjectInfo",
        .label = "Object Info",
        .category = "Input",
        .doc = "Another mesh object's evaluated mesh and transform. Its outMesh and worldMatrix are connected into the modifier, so "
               "editing it re-evaluates this group.",
        .props = {prop("object", {}, "", "Name of the mesh object."),
                  prop("transformSpace", {"original", "world", "instance"}, "original",
                       "original keeps the mesh in its own space; world applies its world matrix; instance passes one instance "
                       "of the mesh placed by its world matrix, which simulation nodes follow as a moving object."),
                  prop("relativeTo", {"world", "owner"}, "world",
                       "world reads the object's transform as it is in the scene; owner reads it relative to the mesh this group "
                       "runs on (where it stood when the object was connected), so moving both together changes nothing.")},
        .sockets = {.outputs = {out("location", T::Vector), out("rotation", T::Rotation), out("scale", T::Vector), geoOut("geometry")}},
        .exec =
            [](ExecContext& c) {
                auto it = c.env.objects.find(c.node.name);
                if (it == c.env.objects.end())
                    return;
                const glm::dmat4 m = c.prop("relativeTo") == "owner" ? glm::inverse(c.env.ownerMatrix) * it->second.matrix : it->second.matrix;
                glm::dvec3 s(glm::length(glm::dvec3(m[0])), glm::length(glm::dvec3(m[1])), glm::length(glm::dvec3(m[2])));
                glm::dmat3 r(glm::dvec3(m[0]) / std::max(s.x, 1e-12), glm::dvec3(m[1]) / std::max(s.y, 1e-12),
                             glm::dvec3(m[2]) / std::max(s.z, 1e-12));
                c.out("location", constantVector(glm::dvec3(m[3])));
                c.out("rotation", constantVector(matrixToEuler(r), T::Rotation));
                c.out("scale", constantVector(s));
                if (it->second.mesh)
                {
                    if (c.prop("transformSpace") == "instance")
                    {
                        GeometryPtr ref;
                        if (it->second.mesh->hasDeleted())
                            ref = meshGeometry(*it->second.mesh);
                        else
                        {
                            Geometry shared;
                            shared.mesh = it->second.mesh;
                            ref = makeGeometry(std::move(shared));
                        }
                        auto inst = std::make_shared<Instances>();
                        inst->references.push_back(std::move(ref));
                        inst->add(m, 0);
                        Geometry g;
                        g.instances = std::move(inst);
                        c.out("geometry", makeGeometry(std::move(g)));
                        return;
                    }
                    geo::Mesh mesh = *it->second.mesh;
                    if (c.prop("transformSpace") == "world")
                        transformMesh(mesh, m);
                    c.out("geometry", meshGeometry(std::move(mesh)));
                }
            },
    });
}
} // namespace

void registerInputNodes()
{
    groupNodes();
    fieldInput("Position", "Position", T::Vector, positionField, "Point positions, or the averaged positions on other domains.");
    fieldInput("Normal", "Normal", T::Vector, normalField, "Surface normal on the evaluated domain.");
    fieldInput("Index", "Index", T::Int, indexField, "Element index on the evaluated domain.");
    fieldInput("ID", "ID", T::Int, idField, "The \"id\" attribute, or the index when there is none.");
    fieldInput("Radius", "Radius", T::Float, radiusField, "The \"radius\" attribute of points and curves (1 when missing).");
    constantInput("Value", "Value", T::Float, 0.5);
    constantInput("Vector", "Vector", T::Vector, vec(0, 0, 0));
    constantInput("Integer", "Integer", T::Int, 0);
    constantInput("Boolean", "Boolean", T::Bool, false);
    constantInput("String", "String", T::String, "");
    randomValue();
    sceneTime();
    objectInfo();
}
} // namespace lila::gn::nodes
