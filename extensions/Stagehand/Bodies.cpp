// Body settings on meshes (the stagehandBody child node and its materials) and the collision shapes made from them.

#include "Stagehand.h"

#include <Decompose.h>

#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

using namespace lila;

namespace stagehand
{
namespace
{
using scene::AttrDef;
using scene::PlugType;

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

/** A concave mesh whose solid fills less than this of its hull collides as convex parts under shape auto. */
constexpr double kHullFill = 0.6;

uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 29);
}

uint64_t bits(double d)
{
    uint64_t u = 0;
    std::memcpy(&u, &d, sizeof u);
    return u;
}

/**
 * Facts about evaluated meshes by identity: an unchanged mesh keeps its pointer between evaluations, so each is hashed
 * and measured once. Expired entries are pruned as the table grows.
 */
class FactTable
{
public:
    MeshFacts get(const scene::MeshPtr& m)
    {
        std::lock_guard lock(mMutex);
        auto it = mTable.find(m.get());
        if (it != mTable.end() && !it->second.first.expired())
            return it->second.second;
        MeshFacts f = measure(*m);
        if (mTable.size() >= mPruneAt)
        {
            std::erase_if(mTable, [](const auto& e) { return e.second.first.expired(); });
            mPruneAt = std::max<size_t>(1024, mTable.size() * 2);
        }
        mTable[m.get()] = {m, f};
        return f;
    }

private:
    static MeshFacts measure(const geo::Mesh& m)
    {
        MeshFacts f;
        uint64_t h = 0x84222325CBF29CE4ull;
        f.lo = glm::dvec3(1e300);
        f.hi = glm::dvec3(-1e300);
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
            {
                const glm::dvec3 q = m.position(p);
                h = mix(mix(mix(mix(h, p), bits(q.x)), bits(q.y)), bits(q.z));
                f.lo = glm::min(f.lo, q);
                f.hi = glm::max(f.hi, q);
                f.any = true;
            }
        for (geo::Index face = 0; face < m.faceCapacity(); ++face)
        {
            if (!m.faceValid(face))
                continue;
            h = mix(h, 0xFACEull + face);
            for (geo::Index p : m.facePoints(face))
                h = mix(h, p);
        }
        f.hash = h;
        if (!f.any)
            f.lo = f.hi = glm::dvec3(0.0);
        return f;
    }

    std::mutex mMutex;
    std::unordered_map<const geo::Mesh*, std::pair<std::weak_ptr<const geo::Mesh>, MeshFacts>> mTable;
    size_t mPruneAt = 1024;
};

FactTable& facts()
{
    static FactTable* t = new FactTable;
    return *t;
}

/** Built collision shapes by what they are made from, the least recently used dropped past the limit. */
class ShapeTable
{
public:
    std::optional<physics::Shape> find(uint64_t key)
    {
        std::lock_guard lock(mMutex);
        const auto it = mMap.find(key);
        if (it == mMap.end())
            return std::nullopt;
        mOrder.splice(mOrder.begin(), mOrder, it->second.second);
        return it->second.first;
    }

    void put(uint64_t key, const physics::Shape& s)
    {
        std::lock_guard lock(mMutex);
        if (mMap.count(key))
            return;
        mOrder.push_front(key);
        mMap.emplace(key, std::make_pair(s, mOrder.begin()));
        while (mMap.size() > kLimit)
        {
            mMap.erase(mOrder.back());
            mOrder.pop_back();
        }
    }

private:
    static constexpr size_t kLimit = 4096;
    std::mutex mMutex;
    std::list<uint64_t> mOrder;
    std::unordered_map<uint64_t, std::pair<physics::Shape, std::list<uint64_t>::iterator>> mMap;
};

ShapeTable& shapes()
{
    static ShapeTable* t = new ShapeTable;
    return *t;
}

/** The mesh's faces as triangles, three points each, scaled. */
std::shared_ptr<std::vector<glm::dvec3>> trianglesOf(const geo::Mesh& mesh, const glm::dvec3& scale)
{
    auto tris = std::make_shared<std::vector<glm::dvec3>>();
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const std::vector<geo::Index> p = mesh.facePoints(f);
        for (size_t k = 1; k + 1 < p.size(); ++k)
            for (geo::Index v : {p[0], p[k], p[k + 1]})
                tris->push_back(mesh.position(v) * scale);
    }
    return tris;
}

/**
 * A static mesh made by a terrain (a geometry nodes modifier producing a heightfield, last in its stack) as a
 * heightfield shape: far cheaper than its triangles. Null when the mesh isn't one, or is turned or sheared so its grid
 * no longer lies flat.
 */
std::optional<physics::Shape> heightfieldShape(Graph& g, NodeId id, const glm::dmat4& world)
{
    const std::vector<NodeId> stack = scene::modifiers::stack(g, id);
    if (stack.empty())
        return std::nullopt;
    Node* top = g.node(stack.back());
    if (!top || !gn::isGnModifier(*top))
        return std::nullopt;
    const std::shared_ptr<const geo::Heightfield> h = gn::modifierHeightfield(top->id());
    if (!h || h->empty())
        return std::nullopt;
    const Frame f = frameOf(world);
    const double turned = 2.0 * std::acos(std::clamp(std::abs(f.rotation.w), 0.0, 1.0));
    if (turned > 1e-6 || f.scale.x <= 0.0 || f.scale.y <= 0.0 || std::abs(f.scale.x - f.scale.z) > 1e-9 * std::abs(f.scale.x))
        return std::nullopt;
    physics::Shape s;
    s.kind = physics::Shape::Kind::Heightfield;
    auto heights = std::make_shared<std::vector<float>>(h->heights);
    if (f.scale.y != 1.0)
        for (float& v : *heights)
            v = float(double(v) * f.scale.y);
    uint64_t key = mix(0x4846ull, uint64_t(h->nx) << 32 | uint64_t(h->nz));
    for (float v : h->heights)
        key = mix(key, std::bit_cast<uint32_t>(v));
    s.heights = std::move(heights);
    s.samplesX = h->nx;
    s.samplesZ = h->nz;
    s.cell = h->cell * f.scale.x;
    s.gridOrigin = h->origin * f.scale.x;
    s.key = mix(mix(mix(key, bits(f.scale.x)), bits(f.scale.y)), bits(h->cell));
    s.key = mix(mix(s.key, bits(h->origin.x)), bits(h->origin.y));
    return s;
}
} // namespace

MeshFacts meshFacts(const scene::MeshPtr& mesh) { return mesh ? facts().get(mesh) : MeshFacts{}; }

bool worldBoundsOf(Graph& g, NodeId id, glm::dvec3& lo, glm::dvec3& hi)
{
    Node* n = g.node(id);
    const int out = n ? n->type().attrIndex("outMesh") : -1;
    if (out < 0)
        return false;
    const auto mesh = n->as<scene::MeshPtr>(out);
    if (!mesh)
        return false;
    const MeshFacts f = meshFacts(mesh);
    if (!f.any)
        return false;
    const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    lo = glm::dvec3(1e300);
    hi = glm::dvec3(-1e300);
    for (int k = 0; k < 8; ++k)
    {
        const glm::dvec3 c(k & 1 ? f.hi.x : f.lo.x, k & 2 ? f.hi.y : f.lo.y, k & 4 ? f.hi.z : f.lo.z);
        const glm::dvec3 q(w * glm::dvec4(c, 1.0));
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    return true;
}

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
    s.shape = ShapeChoice(std::clamp<int64_t>(n->as<int64_t>(t.attrIndex("shape")), 0, 6));
    s.rounding = n->as<double>(t.attrIndex("rounding"));
    s.maxParts = int(std::clamp<int64_t>(n->as<int64_t>(t.attrIndex("maxParts")), 1, 64));
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

physics::Shape shapeOf(Graph& g, NodeId id, ShapeChoice choice, double rounding, bool forStatic, int maxParts)
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
    const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    if (forStatic && (choice == ShapeChoice::Auto || choice == ShapeChoice::Mesh))
        if (std::optional<physics::Shape> h = heightfieldShape(g, id, world))
            return *h;
    return shapeOfMesh(mesh, frameOf(world).scale, choice, rounding, forStatic, maxParts);
}

physics::Shape shapeOfMesh(const scene::MeshPtr& mesh, const glm::dvec3& scale, ShapeChoice choice, double rounding, bool forStatic, int maxParts)
{
    physics::Shape s;
    s.rounding = rounding;
    const MeshFacts facts = meshFacts(mesh);
    if (!facts.any)
        return s;
    uint64_t key = facts.hash;
    for (double d : {scale.x, scale.y, scale.z, rounding})
        key = mix(key, bits(d));
    key = mix(mix(mix(key, uint64_t(choice)), forStatic ? 1 : 0), uint64_t(maxParts));
    if (std::optional<physics::Shape> hit = shapes().find(key))
        return *hit;

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

    if (choice == ShapeChoice::Mesh || (forStatic && choice == ShapeChoice::Auto))
    {
        s.kind = physics::Shape::Kind::Mesh;
        s.points = trianglesOf(*mesh, scale);
    }
    else
    {
        const glm::dvec3 size = hi - lo;
        const glm::dvec3 center = (hi + lo) * 0.5;
        const double boxVolume = size.x * size.y * size.z;
        if (choice == ShapeChoice::Auto)
        {
            const double solid = std::abs(signedVolume(*mesh, scale));
            if (boxVolume > 0.0 && solid >= kBoxFill * boxVolume)
                choice = ShapeChoice::Box;
            else if (solid > 0.0 && solid < kHullFill * physics::hullVolume(*points))
                choice = ShapeChoice::Decompose;
            else
                choice = ShapeChoice::Hull;
        }
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
        case ShapeChoice::Decompose:
        {
            physics::DecomposeOptions o;
            o.maxParts = std::max(1, maxParts);
            s.kind = physics::Shape::Kind::Compound;
            for (std::vector<glm::dvec3>& part : physics::decompose(*trianglesOf(*mesh, scale), o))
                s.parts.push_back(std::make_shared<const std::vector<glm::dvec3>>(std::move(part)));
            s.points = std::move(points);
            break;
        }
        default:
            s.kind = physics::Shape::Kind::Hull;
            s.points = std::move(points);
            break;
        }
    }
    s.key = key;
    shapes().put(key, s);
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
         .doc = "auto: a box when the mesh nearly fills one, convex parts when it is hollow or branching (an arch, a chair), else its "
                "convex hull. decompose: always convex parts. mesh: the exact triangles, for things that never move."},
        {.name = "rounding", .type = PlugType::Float, .defaultValue = 0.2, .doc = "Edge rounding of the collision shape, cm.", .minValue = 0.0,
         .softMax = 2.0},
        {.name = "maxParts", .type = PlugType::Int, .defaultValue = int64_t(16), .doc = "Most convex parts a decomposed shape is cut into.",
         .minValue = 1.0, .maxValue = 64.0},
    };
    t.create = base->create;
    lila::scene::relocate::registerNeutral(t.name);
    return scene::types::registerType(ctx, std::move(t)) != nullptr;
}
} // namespace stagehand
