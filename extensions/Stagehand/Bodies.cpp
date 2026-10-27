// Body settings on meshes (the stagehandBody child node and its materials) and the collision shapes made from them.

#include "Stagehand.h"

#include <lila/Extension.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>

using namespace lila;

namespace stagehand
{
namespace
{
using scene::AttrDef;
using scene::PlugType;

constexpr const char* kShapeNames[] = {"auto", "box", "hull", "sphere", "capsule", "mesh"};
constexpr const char* kRoleNames[] = {"dynamic", "static", "kinematic", "off"};

std::vector<std::string> materialNames()
{
    std::vector<std::string> out;
    for (const Material& m : materials())
        out.emplace_back(m.name);
    return out;
}

/** A box fills at least this much of its bounds to be simulated as the box. */
constexpr double kBoxFill = 0.9;

double signedVolume(const geo::Mesh& m, const glm::dvec3& scale)
{
    double v = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> p = m.facePoints(f);
        for (size_t k = 1; k + 1 < p.size(); ++k)
            v += glm::dot(m.position(p[0]) * scale, glm::cross(m.position(p[k]) * scale, m.position(p[k + 1]) * scale)) / 6.0;
    }
    return v;
}
} // namespace

const std::vector<Material>& materials()
{
    static const std::vector<Material> kMaterials = {
        {"wood", 600.0, 0.6, 0.1, "Planks, shelves, furniture."},
        {"book", 750.0, 0.55, 0.05, "Bound books: dense, a little grippy, barely bounce."},
        {"paper", 500.0, 0.5, 0.0, "Loose paper and card stacks."},
        {"cardboard", 150.0, 0.6, 0.05, "Empty boxes; light, so they tumble far."},
        {"stone", 2400.0, 0.7, 0.05, "Rock, brick, concrete."},
        {"ceramic", 2000.0, 0.4, 0.1, "Plates, mugs, pots."},
        {"metal", 7800.0, 0.4, 0.15, "Steel tools, cans, pipes."},
        {"glass", 2500.0, 0.3, 0.1, "Bottles, jars."},
        {"plastic", 950.0, 0.4, 0.3, "Crates, toys, tubs."},
        {"rubber", 1100.0, 0.9, 0.7, "Balls and tyres."},
        {"fabric", 300.0, 0.8, 0.0, "Folded cloth, cushions (as rigid lumps)."},
    };
    return kMaterials;
}

const Material* material(const std::string& name)
{
    for (const Material& m : materials())
        if (name == m.name)
            return &m;
    return nullptr;
}

Node* bodyNode(Graph& g, NodeId mesh)
{
    for (NodeId c : g.children(mesh))
        if (Node* n = g.node(c); n && n->type().name == kBodyType)
            return n;
    return nullptr;
}

BodySettings bodySettings(Graph& g, NodeId mesh)
{
    BodySettings s;
    Node* n = bodyNode(g, mesh);
    if (!n)
        return s;
    const scene::NodeType& t = n->type();
    s.hasNode = true;
    s.role = RoleChoice(std::clamp<int64_t>(n->as<int64_t>(t.attrIndex("role")), 0, 3));
    const int64_t mat = n->as<int64_t>(t.attrIndex("material"));
    if (mat >= 0 && size_t(mat) < materials().size())
        s.material = materials()[size_t(mat)].name;
    s.density = n->as<double>(t.attrIndex("density"));
    s.friction = n->as<double>(t.attrIndex("friction"));
    s.bounce = n->as<double>(t.attrIndex("bounce"));
    s.shape = ShapeChoice(std::clamp<int64_t>(n->as<int64_t>(t.attrIndex("shape")), 0, 5));
    s.rounding = n->as<double>(t.attrIndex("rounding"));
    return s;
}

Frame frameOf(const glm::dmat4& w)
{
    Frame f;
    f.position = glm::dvec3(w[3]);
    glm::dmat3 r(w);
    f.scale = {glm::length(r[0]), glm::length(r[1]), glm::length(r[2])};
    // A mirrored transform keeps a proper rotation by carrying the flip in its scale.
    if (glm::determinant(r) < 0.0)
        f.scale.x = -f.scale.x;
    for (int c = 0; c < 3; ++c)
        r[c] = std::abs(f.scale[c]) > 1e-12 ? r[c] / f.scale[c] : glm::dvec3(c == 0, c == 1, c == 2);
    f.rotation = glm::normalize(glm::quat_cast(r));
    return f;
}

glm::dmat4 matrixOf(const Frame& f)
{
    glm::dmat4 m = glm::mat4_cast(f.rotation);
    m[0] *= f.scale.x;
    m[1] *= f.scale.y;
    m[2] *= f.scale.z;
    m[3] = glm::dvec4(f.position, 1.0);
    return m;
}

physics::Shape shapeOf(Graph& g, NodeId id, ShapeChoice choice, double rounding, bool forStatic)
{
    LILA_ZONE();

    physics::Shape s;
    s.rounding = rounding;
    Node* n = g.node(id);
    const int out = n ? n->type().attrIndex("outMesh") : -1;
    if (out < 0)
        return s;
    const auto mesh = n->as<scene::MeshPtr>(out);
    if (!mesh)
        return s;
    const glm::dvec3 scale = frameOf(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)).scale;

    auto points = std::make_shared<std::vector<glm::dvec3>>();
    glm::dvec3 lo(1e300), hi(-1e300);
    for (geo::Index p = 0; p < mesh->pointCapacity(); ++p)
        if (mesh->pointValid(p))
        {
            const glm::dvec3 q = mesh->position(p) * scale;
            points->push_back(q);
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
    if (points->empty())
        return s;

    if (choice == ShapeChoice::Mesh || (forStatic && choice == ShapeChoice::Auto))
    {
        auto tris = std::make_shared<std::vector<glm::dvec3>>();
        for (geo::Index f = 0; f < mesh->faceCapacity(); ++f)
        {
            if (!mesh->faceValid(f))
                continue;
            const std::vector<geo::Index> p = mesh->facePoints(f);
            for (size_t k = 1; k + 1 < p.size(); ++k)
                for (geo::Index v : {p[0], p[k], p[k + 1]})
                    tris->push_back(mesh->position(v) * scale);
        }
        s.kind = physics::Shape::Kind::Mesh;
        s.points = std::move(tris);
        return s;
    }

    const glm::dvec3 size = hi - lo;
    const glm::dvec3 center = (hi + lo) * 0.5;
    const double boxVolume = size.x * size.y * size.z;
    if (choice == ShapeChoice::Auto)
        choice = boxVolume > 0.0 && std::abs(signedVolume(*mesh, scale)) >= kBoxFill * boxVolume ? ShapeChoice::Box : ShapeChoice::Hull;

    switch (choice)
    {
    case ShapeChoice::Box:
        s.kind = physics::Shape::Kind::Box;
        s.center = center;
        s.halfExtents = size * 0.5;
        break;
    case ShapeChoice::Sphere:
        s.kind = physics::Shape::Kind::Sphere;
        s.center = center;
        s.radius = std::max({size.x, size.y, size.z}) * 0.5;
        break;
    case ShapeChoice::Capsule:
        s.kind = physics::Shape::Kind::Capsule;
        s.center = center;
        s.radius = std::max(size.x, size.z) * 0.5;
        s.halfHeight = std::max(0.0, size.y * 0.5 - s.radius);
        break;
    default:
        s.kind = physics::Shape::Kind::Hull;
        s.points = std::move(points);
        break;
    }
    return s;
}

bool physical(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    if (!n || !n->type().isA("mesh") || !scene::effectivelyVisible(g, id))
        return false;
    return bodySettings(g, id).role != RoleChoice::Off;
}

void setWorld(Graph& g, NodeId id, const glm::dmat4& world)
{
    Node* n = g.node(id);
    if (!n)
        return;
    const glm::dmat4 parent = n->as<glm::dmat4>(scene::Transform::kParentMatrix);
    scene::Transform::setLocalMatrix(g, id, glm::inverse(parent) * world);
}

bool registerNodes(ext::Context& ctx)
{
    const scene::NodeType* base = scene::types::find("transform");
    if (!base)
        return false;
    const Material& wood = *material("wood");
    scene::NodeType t;
    t.name = kBodyType;
    t.base = base;
    t.dag = true;
    t.icon = "box-model";
    t.doc = "Physics settings for the mesh it sits under: how it takes part when things settle, get pushed or dropped. "
            "Duplicates, references and library assets carry it with the mesh.";
    t.ownAttrs = {
        {.name = "role", .type = PlugType::Enum, .defaultValue = int64_t(0), .enumNames = {std::begin(kRoleNames), std::end(kRoleNames)},
         .doc = "dynamic: moves when pushed or settled. static: only gets hit. kinematic: moves only by hand. off: ignored."},
        {.name = "material", .type = PlugType::Enum, .defaultValue = int64_t(0), .enumNames = materialNames(),
         .doc = "The preset density, friction and bounce came from. Setting it with physicsBody fills them in."},
        {.name = "density", .type = PlugType::Float, .defaultValue = wood.density, .doc = "kg per cubic metre.", .minValue = 1.0, .softMax = 8000.0},
        {.name = "friction", .type = PlugType::Float, .defaultValue = wood.friction, .doc = "0 slides like ice, 1 grips.", .minValue = 0.0, .softMax = 1.0},
        {.name = "bounce", .type = PlugType::Float, .defaultValue = wood.bounce, .doc = "0 lands dead, 1 bounces back as high.", .minValue = 0.0,
         .maxValue = 1.0},
        {.name = "shape", .type = PlugType::Enum, .defaultValue = int64_t(0), .enumNames = {std::begin(kShapeNames), std::end(kShapeNames)},
         .doc = "auto: a box when the mesh nearly fills one, else its convex hull. mesh: the exact triangles, for things that never move."},
        {.name = "rounding", .type = PlugType::Float, .defaultValue = 0.2, .doc = "Edge rounding of the collision shape, cm.", .minValue = 0.0,
         .softMax = 2.0},
    };
    t.create = base->create;
    lila::scene::relocate::registerNeutral(t.name);
    return scene::types::registerType(ctx, std::move(t)) != nullptr;
}
} // namespace stagehand
