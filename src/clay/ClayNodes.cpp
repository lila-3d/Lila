#include "clay/ClayNodes.h"

#include "core/Profile.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Skin.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <map>

namespace lila::clay
{
namespace
{
using namespace scene;
using T = ArgType;

const std::vector<std::string> kShapes = {"sphere", "ellipsoid", "box", "capsule", "cylinder", "cone", "torus"};
const std::vector<std::string> kOps = {"add", "subtract", "intersect"};

int idx(const Node& n, const char* attr) { return n.type().attrIndex(attr); }
double num(const Node& n, const char* attr) { return n.as<double>(idx(n, attr)); }
int64_t choice(const Node& n, const char* attr) { return n.as<int64_t>(idx(n, attr)); }
glm::dvec3 vec(const Node& n, const char* attr) { return n.as<glm::dvec3>(idx(n, attr)); }

AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, std::vector<std::string> options = {},
              std::optional<double> lo = {}, std::optional<double> softHi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .enumNames = std::move(options), .doc = std::move(doc), .minValue = lo, .softMax = softHi};
}

AttrDef vec3(std::string name, const char* parts, glm::dvec3 def, std::string doc, uint32_t extra = 0)
{
    AttrDef a = param(name, PlugType::Vec3, def, std::move(doc));
    a.flags |= extra;
    for (int i = 0; i < 3; ++i)
        a.childNames[size_t(i)] = name + parts[i];
    return a;
}

AttrDef outClay()
{
    return {.name = "outClay", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden,
            .doc = "This item as it joins its parent's clay."};
}

std::vector<AttrDef> joinAttrs()
{
    return {param("operation", PlugType::Enum, int64_t(0), "Add to, subtract from, or intersect with the clay before it.", kOps),
            param("blend", PlugType::Float, 2.0, "Radius of the smooth join with the clay before it, cm; 0 is a hard edge.", {}, 0.0, 20.0)};
}


class PrimitiveNode final : public Locator
{
public:
    void compute(const EvalContext& ctx) override
    {
        Transform::compute(ctx);
        Primitive p{.shape = Shape(std::clamp<int64_t>(choice(*this, "shape"), 0, int64_t(kShapes.size()) - 1)),
                    .extents = vec(*this, "extents"),
                    .radius = num(*this, "radius"),
                    .endRadius = num(*this, "endRadius"),
                    .length = num(*this, "length"),
                    .rounding = num(*this, "rounding"),
                    .color = glm::vec3(vec(*this, "color"))};
        Item item{.op = Op(std::clamp<int64_t>(choice(*this, "operation"), 0, 2)),
                  .blend = num(*this, "blend"),
                  .matrix = as<glm::dmat4>(kWorldMatrix),
                  .shape = p};
        setOutput(idx(*this, "outClay"), DataPtr(std::make_shared<ItemData>(std::move(item))));
    }
};

class GroupNode final : public Transform
{
public:
    void compute(const EvalContext& ctx) override
    {
        Transform::compute(ctx);
        Item item{.op = Op(std::clamp<int64_t>(choice(*this, "operation"), 0, 2)), .blend = num(*this, "blend")};
        setOutput(idx(*this, "outClay"), DataPtr(std::make_shared<ItemData>(std::move(item))));
    }
};

/**
 * The clay tree a surface node has been fed. Every shape and group feeds the surface directly (a group fed by its own
 * children would be a cycle through their parent matrices), with world matrices and each one's depth below the object.
 */
Item rootOf(const Node& surface)
{
    std::map<int, int64_t> depths;
    for (const auto& [k, v] : surface.elements(idx(surface, "depths")))
        depths[k] = std::get<int64_t>(v);
    Item root{.matrix = glm::inverse(surface.as<glm::dmat4>(idx(surface, "objectMatrix")))};
    std::vector<Item*> open{&root};
    for (const auto& [k, v] : surface.elements(idx(surface, "items")))
    {
        const DataPtr* d = std::get_if<DataPtr>(&v);
        if (!d || !*d || std::string_view((*d)->kind()) != "clayItem")
            continue;
        const auto depth = size_t(std::max<int64_t>(depths.contains(k) ? depths[k] : 1, 1));
        while (open.size() > std::min(depth, open.size()))
            open.pop_back();
        open.back()->children.push_back(static_cast<const ItemData&>(**d).item);
        if (!open.back()->children.back().shape)
            open.push_back(&open.back()->children.back());
    }
    return root;
}

class SurfaceNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const Field field(rootOf(*this), choice(*this, "symmetry") == 1);
        const MeshOptions o{.voxelSize = num(*this, "voxelSize"), .sharp = as<bool>(idx(*this, "sharpEdges")), .gpu = as<bool>(idx(*this, "gpu")), .colors = as<bool>(idx(*this, "colors"))};
        std::string warning;
        try
        {
            setOutput(idx(*this, "outMesh"), MeshPtr(mMesher.update(field, o)));
        }
        catch (const std::exception& e)
        {
            warning = e.what();
            mMesher = Mesher();
            setOutput(idx(*this, "outMesh"), MeshPtr(std::make_shared<geo::Mesh>()));
        }
        setOutput(idx(*this, "warning"), warning);
    }

private:
    Mesher mMesher;
};

// ------------------------------------------------------------------------------------------------ commands

bool isItem(const Node& n) { return n.type().isA("clayPrimitive") || n.type().isA("clayGroup"); }

/** The mesh under a clay object that shows its surface. */
Node* meshOf(Graph& g, const Node& object)
{
    if (!object.type().isA("clay"))
        return nullptr;
    for (NodeId c : g.children(object.id()))
        if (Node* n = g.node(c); n && n->type().isA("mesh"))
            if (std::optional<PlugRef> src = g.source({c, MeshNode::kInMesh, -1}); src && g.node(src->node)->type().isA("claySurface"))
                return n;
    return nullptr;
}

Node* surfaceOfObject(Graph& g, const Node& object)
{
    Node* mesh = meshOf(g, object);
    return mesh ? g.node(g.source({mesh->id(), MeshNode::kInMesh, -1})->node) : nullptr;
}

void collect(Graph& g, NodeId parent, int64_t depth, std::vector<std::pair<NodeId, int64_t>>& out)
{
    for (NodeId c : g.children(parent))
        if (Node* child = g.node(c); child && isItem(*child))
        {
            out.emplace_back(c, depth);
            if (child->type().isA("clayGroup"))
                collect(g, c, depth + 1, out);
        }
}

/** Rewires a clay object's surface to its shapes and groups in outliner order, which is the order they join in. */
void sync(Graph& g, NodeId object)
{
    Node* o = g.node(object);
    Node* s = o ? surfaceOfObject(g, *o) : nullptr;
    if (!s)
        return;
    const PlugRef items{s->id(), idx(*s, "items")};
    for (const PlugRef& e : g.elements(items))
        g.removeElement(e);
    std::vector<std::pair<NodeId, int64_t>> tree;
    collect(g, object, 1, tree);
    std::vector<PlugValue> depths;
    for (const auto& [id, depth] : tree)
    {
        g.connect({id, idx(*g.node(id), "outClay")}, {items.node, items.attr, -1, int(depths.size())});
        depths.emplace_back(depth);
    }
    g.setElements({s->id(), idx(*s, "depths")}, depths);
}

/** The clay object id is part of: itself, or its nearest clay ancestor. */
Node* objectOfNode(Graph& g, NodeId id)
{
    for (NodeId at = id; at != kNoNode; at = g.parent(at))
        if (Node* n = g.node(at); n && n->type().isA("clay"))
            return n;
    return nullptr;
}

Node& named(Graph& g, const std::string& path, const char* cmd)
{
    Node* n = g.find(path);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named " + path);
    return *n;
}

/** The positional nodes, or the selection when none were given. */
std::vector<Node*> targets(Graph& g, const Args& a, const char* cmd)
{
    std::vector<Node*> out;
    for (const json& p : a.positional)
        out.push_back(&named(g, p.get<std::string>(), cmd));
    if (out.empty())
        for (NodeId id : g.selection())
            if (Node* n = g.node(id))
                out.push_back(n);
    return out;
}

void setAttrs(Graph& g, Node& n, const Args& a, const char* cmd)
{
    for (const auto& [flag, value] : a.flags.items())
    {
        const int at = n.type().attrIndex(flag);
        if (at < 0 || value.is_null() || (n.type().attrs[size_t(at)].flags & AttrOutput))
            continue;
        try
        {
            g.setValue({n.id(), at, -1}, fromJson(n.type().attrs[size_t(at)], value));
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string(cmd) + ": " + flag + ": " + e.what());
        }
    }
}

glm::dvec3 vecFlag(const Args& a, const char* flag) { return {a.flags[flag][0], a.flags[flag][1], a.flags[flag][2]}; }

std::vector<FlagDef> shapeFlags()
{
    return {
        {"shape", "sh", T::String, "sphere", "Primitive shape.", kShapes},
        {"operation", "op", T::String, "add", "Add to, subtract from, or intersect with the clay before it.", kOps},
        {"blend", "b", T::Float, nullptr, "Smooth join radius, cm; 0 is a hard edge.", {}, {}, 0.0},
        {"radius", "r", T::Float, nullptr, "Sphere radius; capsule and cone radius at -Y; cylinder radius; torus ring radius. cm."},
        {"endRadius", "er", T::Float, nullptr, "Capsule and cone radius at +Y; torus tube radius. cm."},
        {"length", "l", T::Float, nullptr, "Capsule, cylinder and cone length along Y, cm."},
        {"extents", "ex", T::Vec3, nullptr, "Box half sizes, ellipsoid semi-axes, cm."},
        {"rounding", "rn", T::Float, nullptr, "Box, cylinder and cone edge rounding, cm."},
        {"color", "c", T::Vec3, nullptr, "Linear colour, written to the clay color set."},
        {"position", "p", T::Vec3, nullptr, "World position."},
        {"rotation", "ro", T::Vec3, nullptr, "Rotation in degrees."},
        {"name", "n", T::String, nullptr, "Name; made unique."},
    };
}

glm::dmat4 worldOf(Graph& g, NodeId id) { return std::get<glm::dmat4>(g.value({id, Transform::kWorldMatrix, -1})); }

/**
 * A capsule chain through world points, as round cones in a new group under parent: one per span, the radius running
 * from radius at the first point to endRadius at the last. Consecutive cones share their end spheres, so the chain is
 * smooth without a blend.
 */
Node* addStroke(Graph& g, Node& parent, const std::vector<glm::dvec3>& points, double radius, double endRadius, const Args& a, Node* into)
{
    Node* group = into ? into : g.createNode(*types::find("clayGroup"), a.get<std::string>("name", "ClayStroke"), parent.id(), true);
    if (!into)
        setAttrs(g, *group, a, "addClayStroke");
    g.evaluate();
    const glm::dmat4 toGroup = glm::inverse(worldOf(g, group->id()));
    double total = 0.0;
    for (size_t i = 1; i < points.size(); ++i)
        total += glm::length(points[i] - points[i - 1]);
    double along = 0.0;
    for (size_t i = 1; i < points.size(); ++i)
    {
        const glm::dvec3 from = points[i - 1], to = points[i];
        const double len = glm::length(to - from);
        if (len < 1e-9)
            continue;
        const double r0 = glm::mix(radius, endRadius, total > 0.0 ? along / total : 0.0);
        along += len;
        const double r1 = glm::mix(radius, endRadius, total > 0.0 ? along / total : 1.0);
        Node* p = g.createNode(*types::find("clayPrimitive"), "ClaySpan", group->id(), true);
        auto set = [&](const char* attr, PlugValue v) { g.setValue({p->id(), idx(*p, attr), -1}, std::move(v)); };
        set("shape", int64_t(Shape::Capsule));
        set("blend", 0.0);
        set("radius", r0);
        set("endRadius", r1);
        set("length", len);
        if (a.has("color"))
            set("color", vecFlag(a, "color"));
        const glm::dmat4 world = glm::translate(glm::dmat4(1.0), 0.5 * (from + to)) *
                                 glm::toMat4(glm::rotation(glm::dvec3(0, 1, 0), (to - from) / len));
        Transform::setLocalMatrix(g, p->id(), toGroup * world);
    }
    return group;
}

/** The shape of a clay object nearest p (world), deepest inside first; cuts count, so their carved surfaces pick them. */
Node* shapeAt(Graph& g, NodeId object, const glm::dvec3& p)
{
    g.evaluate();
    std::vector<std::pair<NodeId, int64_t>> tree;
    collect(g, object, 1, tree);
    Node* best = nullptr;
    double nearest = 1e300;
    for (const auto& [id, depth] : tree)
    {
        Node* n = g.node(id);
        const DataPtr d = n->as<DataPtr>(idx(*n, "outClay"));
        if (!d || !n->type().isA("clayPrimitive"))
            continue;
        const Item& item = static_cast<const ItemData&>(*d).item;
        const glm::dmat4& m = item.matrix;
        const double scale = std::min({glm::length(glm::dvec3(m[0])), glm::length(glm::dvec3(m[1])), glm::length(glm::dvec3(m[2]))});
        const double dist = distance(*item.shape, glm::dvec3(glm::inverse(m) * glm::dvec4(p, 1.0))) * scale;
        if (dist < nearest)
        {
            nearest = dist;
            best = n;
        }
    }
    return best;
}

/** A clay shape in world space, ready for distances. */
struct Placed
{
    Primitive shape;
    glm::dmat4 toLocal{1.0};
    double scale = 1.0;
    NodeId joint = kNoNode;

    double distanceTo(const glm::dvec3& p) const { return distance(shape, glm::dvec3(toLocal * glm::dvec4(p, 1.0))) * scale; }
};

Placed placed(const Node& n)
{
    const Item& item = static_cast<const ItemData&>(*n.as<DataPtr>(idx(n, "outClay"))).item;
    const glm::dmat4& m = item.matrix;
    return {*item.shape, glm::inverse(m), std::min({glm::length(glm::dvec3(m[0])), glm::length(glm::dvec3(m[1])), glm::length(glm::dvec3(m[2]))})};
}

/** A group of capsules only: a stroke, which gets a joint chain. */
bool isStroke(Graph& g, NodeId group)
{
    const auto& kids = g.children(group);
    return !kids.empty() && std::ranges::all_of(kids, [&](NodeId c) {
        const Node* n = g.node(c);
        return n && n->type().isA("clayPrimitive") && n->as<int64_t>(idx(*n, "shape")) == int64_t(Shape::Capsule);
    });
}

/**
 * Starting skin weights from the clay: each point leans on the joints of the shapes it lies nearest, falling off with
 * how much further each one is than the nearest.
 */
void weightFromClay(Graph& g, NodeId skinId, const std::vector<Placed>& shapes, int maxInfluences)
{
    const MeshPtr input = skin::inputMesh(g, skinId);
    const NodeId meshId = skin::meshOf(g, skinId);
    if (!input || meshId == kNoNode)
        return;
    std::map<NodeId, uint32_t> influence;
    for (const skin::Influence& inf : skin::influences(g, skinId))
        influence[inf.joint] = inf.index;
    const glm::dmat4 world = worldOf(g, meshId);
    // How far past the nearest shape a joint's weight fades: a little of the surface's own scale.
    const double soft = std::max(0.25, 0.005 * glm::length(input->bounds().max - input->bounds().min));
    WeightTable t;
    t.points.resize(input->pointCapacity());
    for (geo::Index p = 0; p < input->pointCapacity(); ++p)
    {
        if (!input->pointValid(p))
            continue;
        const glm::dvec3 at(world * glm::dvec4(input->position(p), 1.0));
        std::map<NodeId, double> nearest;
        double best = 1e300;
        for (const Placed& s : shapes)
        {
            const double d = s.distanceTo(at);
            auto [it, fresh] = nearest.try_emplace(s.joint, d);
            if (!fresh)
                it->second = std::min(it->second, d);
            best = std::min(best, d);
        }
        std::vector<std::pair<uint32_t, float>> w;
        for (const auto& [joint, d] : nearest)
            if (auto it = influence.find(joint); it != influence.end())
                w.emplace_back(it->second, float(1.0 / std::pow(std::max(d - best, 0.0) + soft, 4.0)));
        std::ranges::sort(w, [](const auto& a, const auto& b) { return a.second > b.second; });
        if (w.size() > size_t(maxInfluences))
            w.resize(size_t(maxInfluences));
        float sum = 0.0f;
        for (const auto& e : w)
            sum += e.second;
        for (auto& e : w)
            e.second /= std::max(sum, 1e-20f);
        t.points[p] = std::move(w);
    }
    skin::setWeights(g, skinId, t);
}

json addPrimitive(Graph& g, Node& parent, const Args& a)
{
    Node* p = g.createNode(*types::find("clayPrimitive"), a.get<std::string>("name", "ClayPrimitive"), parent.id(), true);
    setAttrs(g, *p, a, "addClayPrimitive");
    if (a.has("rotation"))
        g.setValue({p->id(), Transform::kRotate, -1}, vecFlag(a, "rotation"));
    if (a.has("position"))
    {
        g.evaluate();
        const glm::dmat4 parentWorld = std::get<glm::dmat4>(g.value({parent.id(), Transform::kWorldMatrix, -1}));
        g.setValue({p->id(), Transform::kTranslate, -1}, glm::dvec3(glm::inverse(parentWorld) * glm::dvec4(vecFlag(a, "position"), 1.0)));
    }
    sync(g, objectOfNode(g, parent.id())->id());
    g.select({p->id()});
    return p->path();
}

/** Where new clay goes for a target: a clay object or group itself, a shape's parent, a clay mesh's object. */
Node& containerOf(Graph& g, Node& n, const char* cmd)
{
    if (n.type().isA("clayGroup") || n.type().isA("clay"))
        return n;
    if (Node* p = g.node(g.parent(n.id())); p && (n.type().isA("clayPrimitive") || (p->type().isA("clay") && meshOf(g, *p) == &n)))
        return *p;
    throw CommandError(std::string(cmd) + ": " + n.path() + " is not clay");
}
} // namespace

Node* surfaceOf(const Node& object) { return clay::surfaceOfObject(Graph::current(), object); }
Node* objectOf(NodeId id) { return clay::objectOfNode(Graph::current(), id); }

void registerTypes()
{
    if (types::find("claySurface"))
        return;

    NodeType object;
    object.name = "clay";
    object.icon = "droplet";
    object.base = types::find("transform");
    object.dag = true;
    object.doc = "A clay object: its clay shapes and groups, in join order, and the mesh that shows them blended.";
    object.create = [] { return std::make_unique<Transform>(); };
    types::registerType(std::move(object));

    NodeType prim;
    prim.name = "clayPrimitive";
    prim.icon = "circle-dot";
    prim.base = types::find("locator");
    prim.dag = true;
    prim.doc = "A shape in a clay object: a sphere, ellipsoid, box, capsule, cylinder, cone or torus that adds to, cuts or "
               "intersects the clay before it with a smooth join. Order among siblings is the order they join in.";
    prim.ownAttrs = {param("shape", PlugType::Enum, int64_t(0), "The primitive's shape.", kShapes)};
    for (AttrDef& d : joinAttrs())
        prim.ownAttrs.push_back(std::move(d));
    prim.ownAttrs.push_back(param("radius", PlugType::Float, 10.0, "Sphere radius; capsule and cone radius at -Y; cylinder radius; torus ring radius. cm.", {}, 0.0, 100.0));
    prim.ownAttrs.push_back(param("endRadius", PlugType::Float, 10.0, "Capsule and cone radius at +Y; torus tube radius. cm.", {}, 0.0, 100.0));
    prim.ownAttrs.push_back(param("length", PlugType::Float, 20.0, "Capsule, cylinder and cone length along Y, cm.", {}, 0.0, 200.0));
    prim.ownAttrs.push_back(vec3("extents", "XYZ", glm::dvec3(10.0), "Box half sizes and ellipsoid semi-axes, cm."));
    prim.ownAttrs.push_back(param("rounding", PlugType::Float, 0.0, "Box, cylinder and cone edge rounding, cm.", {}, 0.0, 20.0));
    prim.ownAttrs.push_back(vec3("color", "RGB", glm::dvec3(1.0), "Linear colour, blended across joins into the clay color set.", AttrColor));
    prim.ownAttrs.push_back(outClay());
    prim.create = [] { return std::make_unique<PrimitiveNode>(); };
    types::registerType(std::move(prim));

    NodeType group;
    group.name = "clayGroup";
    group.icon = "box-multiple";
    group.base = types::find("transform");
    group.dag = true;
    group.doc = "Clay shapes that join each other first and then join the clay before the group as one, so a cut inside "
                "reaches only the group.";
    group.ownAttrs = joinAttrs();
    group.ownAttrs.push_back(outClay());
    group.create = [] { return std::make_unique<GroupNode>(); };
    types::registerType(std::move(group));

    NodeType surface;
    surface.name = "claySurface";
    surface.icon = "grid-dots";
    surface.doc = "Meshes a clay object's shapes: signed distances joined smoothly, surfaced with quads on a voxel lattice. "
                  "Only the part of the surface an edit reaches is rebuilt.";
    surface.ownAttrs = {
        {.name = "items", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrArray | AttrHidden,
         .doc = "The clay object's shapes and groups, depth first in join order, kept by the clay commands."},
        {.name = "depths", .type = PlugType::Int, .defaultValue = int64_t(1), .flags = AttrInput | AttrStorable | AttrArray | AttrHidden,
         .doc = "How deep each of items sits below the clay object: 1 for its children, 2 inside a group, and so on."},
        {.name = "objectMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
         .doc = "The clay object's worldMatrix, so the surface is built in its space."},
        param("voxelSize", PlugType::Float, 0.5, "Lattice spacing while editing, cm. Smaller is finer and slower.", {}, 0.01, 5.0),
        param("finalVoxelSize", PlugType::Float, 0.25, "Lattice spacing convertClayToMesh uses, cm.", {}, 0.01, 5.0),
        param("symmetry", PlugType::Enum, int64_t(0), "Mirror the clay across the object's YZ plane.", {"off", "x"}),
        param("sharpEdges", PlugType::Bool, false, "Keep hard joins and box edges sharp (dual contouring). For hard-surface clay; slower."),
        param("colors", PlugType::Bool, true, "Write the shapes' colours to the clay color set."),
        param("gpu", PlugType::Bool, true, "Sample the surface on the GPU when one is available; off meshes on the CPU."),
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden, .doc = "The clay surface."},
    };
    surface.create = [] { return std::make_unique<SurfaceNode>(); };
    types::registerType(std::move(surface));
    registerGnNodes();
}

void registerCommands()
{
    ops::registerCommand({
        .name = "createClay",
        .doc = "Create a clay object: a transform holding clay shapes, and a mesh under it that shows them blended by a "
               "claySurface. Starts with one sphere unless empty. Returns [object, mesh, surface, firstShape or None].",
        .flags = {{"name", "n", T::String, "Clay", "Object name; made unique."},
                  {"position", "p", T::Vec3, nullptr, "World position."},
                  {"voxelSize", "vs", T::Float, nullptr, "Lattice spacing while editing, cm.", {}, {}, 0.01},
                  {"symmetry", "sy", T::String, nullptr, "Mirror across the object's YZ plane.", {"off", "x"}},
                  {"empty", "e", T::Bool, false, "Start with no shapes."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node* surface = g.createNode(*types::find("claySurface"), {}, kNoNode, true);
            setAttrs(g, *surface, a, "createClay");
            Node* object = g.createNode(*types::find("clay"), a.get<std::string>("name", "Clay"), kNoNode, true);
            Node* mesh = g.createNode(*types::find("mesh"), object->name() + "Surface", object->id(), true);
            g.connect({surface->id(), idx(*surface, "outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1});
            g.connect({object->id(), Transform::kWorldMatrix, -1}, {surface->id(), idx(*surface, "objectMatrix"), -1});
            if (a.has("position"))
                g.setValue({object->id(), Transform::kTranslate, -1}, vecFlag(a, "position"));
            json first = nullptr;
            if (!a.get<bool>("empty", false))
                first = addPrimitive(g, *object, Args{});
            g.select({object->id()});
            return json::array({object->path(), mesh->path(), surface->path(), first});
        },
        .examples = {"createClay(name=\"Dog\", symmetry=\"x\")"},
    });

    ops::registerCommand({
        .name = "addClayPrimitive",
        .doc = "Add a clay shape to a clay object or group, after its other shapes. Given a shape, adds next to it. Returns the new shape.",
        .positionalDoc = "clay object, group or shape",
        .flags = shapeFlags(),
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> t = targets(g, a, "addClayPrimitive");
            if (t.empty())
                throw CommandError("addClayPrimitive: select a clay object, group or shape");
            return addPrimitive(g, containerOf(g, *t.front(), "addClayPrimitive"), a);
        },
        .adjustable = true,
        .examples = {"addClayPrimitive(\"Dog\", shape=\"capsule\", radius=4, endRadius=3, length=20, position=[8, 10, 0], rotation=[0, 0, 90])",
                     "addClayPrimitive(\"Dog\", shape=\"sphere\", operation=\"subtract\", radius=2, blend=0.5)"},
    });

    ops::registerCommand({
        .name = "groupClay",
        .doc = "Group clay shapes that share a parent, so they join each other first and then the clay before them as one. "
               "The group takes the first shape's place in the join order. Returns the group.",
        .positionalDoc = "shapes",
        .flags = {{"operation", "op", T::String, "add", "How the group joins the clay before it.", kOps},
                  {"blend", "b", T::Float, nullptr, "Smooth join radius of the group, cm.", {}, {}, 0.0},
                  {"name", "n", T::String, "ClayGroup", "Group name; made unique."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> items = targets(g, a, "groupClay");
            std::erase_if(items, [](const Node* n) { return !isItem(*n); });
            if (items.empty())
                throw CommandError("groupClay: select clay shapes");
            const NodeId parent = g.parent(items.front()->id());
            if (std::ranges::any_of(items, [&](const Node* n) { return g.parent(n->id()) != parent; }))
                throw CommandError("groupClay: the shapes must share a parent");
            const auto& siblings = g.children(parent);
            size_t at = siblings.size();
            for (const Node* n : items)
                at = std::min(at, size_t(std::ranges::find(siblings, n->id()) - siblings.begin()));
            Node* group = g.createNode(*types::find("clayGroup"), a.get<std::string>("name", "ClayGroup"), parent, true);
            setAttrs(g, *group, a, "groupClay");
            g.setParent(group->id(), parent, false, at);
            for (Node* n : items)
                g.setParent(n->id(), group->id(), true);
            sync(g, objectOfNode(g, parent)->id());
            g.select({group->id()});
            return group->path();
        },
        .examples = {"groupClay([\"Ear\", \"EarCut\"], blend=1)"},
    });

    ops::registerCommand({
        .name = "addClayStroke",
        .doc = "Add a capsule chain through world points to a clay object or group: tentacles, horns, limbs and strokes drawn "
               "over the surface. The spans go in a new group. Returns the group.",
        .positionalDoc = "clay object, group or shape",
        .flags = {{"points", "pts", T::List, nullptr, "World points, two or more: [[x, y, z], ...]."},
                  {"radius", "r", T::Float, 2.0, "Radius at the first point, cm.", {}, {}, 0.0},
                  {"endRadius", "er", T::Float, nullptr, "Radius at the last point, cm; defaults to radius.", {}, {}, 0.0},
                  {"operation", "op", T::String, "add", "How the stroke joins the clay before it.", kOps},
                  {"blend", "b", T::Float, nullptr, "Smooth join radius of the stroke, cm.", {}, {}, 0.0},
                  {"color", "c", T::Vec3, nullptr, "Linear colour."},
                  {"extend", "x", T::Bool, false, "Given a stroke group (or one of its spans), add the spans to it rather than to a new group."},
                  {"name", "n", T::String, nullptr, "Group name; made unique."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> t = targets(g, a, "addClayStroke");
            if (t.empty())
                throw CommandError("addClayStroke: select a clay object, group or shape");
            std::vector<glm::dvec3> pts;
            if (a.has("points") && a.flags["points"].is_array())
                for (const json& p : a.flags["points"])
                    pts.emplace_back(p.at(0).get<double>(), p.at(1).get<double>(), p.at(2).get<double>());
            if (pts.size() < 2)
                throw CommandError("addClayStroke: give two or more points");
            Node* into = nullptr;
            if (a.get<bool>("extend", false))
                into = t.front()->type().isA("clayGroup") ? t.front() : g.node(g.parent(t.front()->id()));
            if (into && !into->type().isA("clayGroup"))
                throw CommandError("addClayStroke: extend needs a stroke group or one of its spans");
            Node& parent = into ? *g.node(g.parent(into->id())) : containerOf(g, *t.front(), "addClayStroke");
            const double r = std::max(0.0, a.get<double>("radius", 2.0));
            Node* group = addStroke(g, parent, pts, r, std::max(0.0, a.get<double>("endRadius", r)), a, into);
            sync(g, objectOfNode(g, parent.id())->id());
            g.select({group->id()});
            return group->path();
        },
        .examples = {"addClayStroke(\"Dog\", points=[[0, 20, -10], [0, 24, -18], [0, 30, -22]], radius=2, endRadius=0.5)"},
    });

    ops::registerCommand({
        .name = "clayShapeAt",
        .doc = "The clay shape of a clay object at a world point: the one the point is deepest inside, else the nearest. Cuts "
               "count. Returns its path, or None for an object without shapes.",
        .positionalDoc = "clay object or anything in it",
        .flags = {{"point", "p", T::Vec3, nullptr, "World point."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> t = targets(g, a, "clayShapeAt");
            Node* object = t.empty() ? nullptr : objectOfNode(g, t.front()->id());
            if (!object || !a.has("point"))
                throw CommandError("clayShapeAt: give a clay object and a point");
            Node* n = shapeAt(g, object->id(), vecFlag(a, "point"));
            return n ? json(n->path()) : json(nullptr);
        },
        .examples = {"clayShapeAt(\"Dog\", point=[3, 20, 8])"},
    });

    ops::registerCommand({
        .name = "clayJoints",
        .doc = "Joints from a clay object: a root at the object for the body, and a chain down each stroke (a group of capsules). "
               "Given a mesh (the converted clay), binds it and starts its weights from the clay: each point leans on the "
               "joints of the shapes it lies nearest. Returns [root, skinCluster or None].",
        .positionalDoc = "clay object",
        .flags = {{"mesh", "m", T::String, nullptr, "Mesh to bind, usually the clay converted with convertClayToMesh."},
                  {"maxInfluences", "mi", T::Int, 4, "Influences per point.", {}, {}, 1.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> t = targets(g, a, "clayJoints");
            Node* object = t.empty() ? nullptr : objectOfNode(g, t.front()->id());
            if (!object)
                throw CommandError("clayJoints: select a clay object");
            g.evaluate();
            auto vec = [](const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); };
            const std::string root = ops::execute("joint", {{"position", vec(glm::dvec3(worldOf(g, object->id())[3]))},
                                                            {"name", object->name() + "_root"}, {"parent", ""}, {"skipSelect", true}});
            const NodeId rootId = g.find(root)->id();

            std::vector<std::pair<NodeId, int64_t>> tree;
            collect(g, object->id(), 1, tree);
            std::vector<Placed> shapes;
            std::vector<NodeId> strokes;
            for (const auto& [id, depth] : tree)
            {
                Node* n = g.node(id);
                if (n->type().isA("clayGroup") && isStroke(g, id))
                    strokes.push_back(id);
            }
            for (const auto& [id, depth] : tree)
            {
                Node* n = g.node(id);
                const bool inStroke = std::ranges::find(strokes, g.parent(id)) != strokes.end();
                if (n->type().isA("clayPrimitive") && !inStroke && n->as<int64_t>(idx(*n, "operation")) == 0)
                {
                    shapes.push_back(placed(*n));
                    shapes.back().joint = rootId;
                }
            }
            for (NodeId stroke : strokes)
            {
                std::string parent = root;
                const auto& spans = g.children(stroke);
                int k = 0;
                for (size_t i = 0; i <= spans.size(); ++i)
                {
                    // A joint at each span's start, and one at the last span's end.
                    const Node& span = *g.node(spans[std::min(i, spans.size() - 1)]);
                    const double half = 0.5 * span.as<double>(idx(span, "length"));
                    const glm::dvec3 at(worldOf(g, span.id()) * glm::dvec4(0.0, i < spans.size() ? -half : half, 0.0, 1.0));
                    parent = ops::execute("joint", {{"position", vec(at)}, {"name", g.node(stroke)->name() + "_" + std::to_string(++k)},
                                                    {"parent", parent}, {"skipSelect", true}});
                    if (i < spans.size())
                    {
                        shapes.push_back(placed(span));
                        shapes.back().joint = g.find(parent)->id();
                    }
                }
            }
            ops::execute("orientJoint", {{"args", json::array({root})}});

            json skinName = nullptr;
            if (a.has("mesh"))
            {
                const std::string mesh = a.get<std::string>("mesh");
                skinName = ops::execute("skinCluster", {{"args", json::array({root, mesh})}, {"maxInfluences", a.get<int>("maxInfluences", 4)}});
                weightFromClay(g, skin::resolve(g, mesh, "clayJoints"), shapes, std::max(1, a.get<int>("maxInfluences", 4)));
            }
            g.select({rootId});
            return json::array({g.node(rootId)->path(), skinName});
        },
        .examples = {"mesh = convertClayToMesh(\"Dog\")\nclayJoints(\"Dog\", mesh=mesh)"},
    });

    ops::registerCommand({
        .name = "refreshClay",
        .doc = "Rewire clay objects to their shapes after shapes were reordered or reparented outside the clay commands. "
               "Siblings join in outliner order. Returns the objects refreshed.",
        .positionalDoc = "clay objects or anything in them",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            std::vector<NodeId> done;
            for (Node* n : targets(g, a, "refreshClay"))
                if (Node* object = objectOfNode(g, n->id()); object && std::ranges::find(done, object->id()) == done.end())
                {
                    done.push_back(object->id());
                    sync(g, object->id());
                    out.push_back(object->path());
                }
            if (out.empty())
                throw CommandError("refreshClay: select a clay object");
            return out;
        },
        .examples = {"refreshClay(\"Dog\")"},
    });

    ops::registerCommand({
        .name = "convertClayToMesh",
        .doc = "Bake a clay object into a plain mesh at the surface's finalVoxelSize, optionally quad remeshed. The clay object "
               "stays, hidden, unless keepSource is off. Returns the new mesh.",
        .positionalDoc = "clay object",
        .flags = {{"voxelSize", "vs", T::Float, nullptr, "Lattice spacing, cm; defaults to the surface's finalVoxelSize.", {}, {}, 0.01},
                  {"keepSource", "ks", T::Bool, true, "Keep the clay object, hidden, to edit and convert again; off deletes it."},
                  {"remesh", "rm", T::Bool, false, "Quad remesh the result."},
                  {"targetFaces", "tf", T::Int, 2000, "Face count for remesh.", {}, {}, 8.0},
                  {"name", "n", T::String, nullptr, "Mesh name; made unique. Defaults to the clay object's name."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> t = targets(g, a, "convertClayToMesh");
            Node* object = t.empty() ? nullptr : objectOfNode(g, t.front()->id());
            if (!object)
                throw CommandError("convertClayToMesh: select a clay object");
            Node* surface = surfaceOfObject(g, *object);
            g.evaluate();
            const Field field(rootOf(*surface), choice(*surface, "symmetry") == 1);
            MeshOptions o{.voxelSize = a.get<double>("voxelSize", num(*surface, "finalVoxelSize")),
                          .sharp = surface->as<bool>(idx(*surface, "sharpEdges")),
                          .gpu = surface->as<bool>(idx(*surface, "gpu")),
                          .colors = surface->as<bool>(idx(*surface, "colors"))};
            geo::Mesh m;
            try
            {
                m = clay::mesh(field, o);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("convertClayToMesh: ") + e.what());
            }
            const glm::dmat4 world = std::get<glm::dmat4>(g.value({object->id(), Transform::kWorldMatrix, -1}));
            const std::string name = a.get<std::string>("name", object->name());
            Node* out = g.createNode(*types::find("mesh"), name, kNoNode, true);
            g.setValue({out->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            Transform::setLocalMatrix(g, out->id(), world);
            if (a.get<bool>("keepSource", true))
                g.setValue({object->id(), Transform::kVisibility, -1}, false);
            else
            {
                const NodeId surfaceId = surface->id();
                g.deleteNode(object->id());
                if (g.node(surfaceId))
                    g.deleteNode(surfaceId);
            }
            g.select({out->id()});
            if (a.get<bool>("remesh", false))
                return ops::execute("quadRemesh", {{"args", json::array({out->path()})}, {"targetFaces", a.get<int>("targetFaces", 2000)},
                                                   {"keepSource", false}});
            return out->path();
        },
        .examples = {"convertClayToMesh(\"Dog\")", "convertClayToMesh(\"Dog\", remesh=True, targetFaces=3000)"},
    });

    using ops::options::Override;
    Override shape;
    shape.order = {"shape", "operation", "blend", "radius", "endRadius", "length", "extents", "rounding", "color"};
    shape.hidden = {"name", "position", "rotation"};
    shape.sections = {{"radius", "Size"}, {"endRadius", "Size"}, {"length", "Size"}, {"extents", "Size"}, {"rounding", "Size"}};
    shape.ranges = {{"blend", {0.0, 50.0, 0.5, " cm"}},   {"radius", {0.0, 500.0, 1.0, " cm"}},  {"endRadius", {0.0, 500.0, 1.0, " cm"}},
                    {"length", {0.0, 1000.0, 1.0, " cm"}}, {"rounding", {0.0, 100.0, 0.5, " cm"}}};
    shape.adjust = {"extents", "color"};
    shape.docs = "clay";
    ops::options::setOverride("addClayPrimitive", std::move(shape));

    Override create;
    create.hidden = {"name", "position"};
    create.ranges = {{"voxelSize", {0.05, 5.0, 0.05, " cm"}}};
    create.docs = "clay";
    ops::options::setOverride("createClay", std::move(create));

    Override group;
    group.hidden = {"name"};
    group.ranges = {{"blend", {0.0, 50.0, 0.5, " cm"}}};
    group.docs = "clay";
    ops::options::setOverride("groupClay", std::move(group));

    Override convert;
    convert.order = {"voxelSize", "remesh", "targetFaces", "keepSource"};
    convert.hidden = {"name"};
    convert.ranges = {{"voxelSize", {0.05, 5.0, 0.05, " cm"}}, {"targetFaces", {8.0, 200000.0, 500.0, ""}}};
    convert.docs = "clay";
    ops::options::setOverride("convertClayToMesh", std::move(convert));
}
} // namespace lila::clay
