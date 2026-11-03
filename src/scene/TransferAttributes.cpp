#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Primitives.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "uv/Guided.h"

#include <tbb/parallel_for.h>

#include <sstream>

namespace lila::scene
{
namespace
{
using geo::Index;

enum class TransferSpace
{
    World,
    Object,
    Component,
    Uv,
    Guided,
};

enum class Search
{
    Closest,
    AlongNormal,
};

const std::vector<std::string> kSpaceNames = {"world", "object", "component", "uv", "guided"};
const std::vector<std::string> kSearchNames = {"closest", "normal"};
const std::vector<std::string> kFlipNames = {"none", "x", "y", "z"};

struct Options
{
    bool uvs = true;
    bool normals = false;
    bool colors = false;
    bool positions = false;
    std::vector<std::string> attributes;
    TransferSpace space = TransferSpace::World;
    Search search = Search::Closest;
    double maxDistance = -1.0;
    int flipAxis = -1; // mirror sample points across this object-space axis of the source
    int sourceUvSet = -1; // -1: every set onto the same index
    int targetUvSet = -1; // -1: the source set's own index
    std::string sourceColorSet; // empty: every set onto the same name
    std::string targetColorSet; // empty: the source set's own name
    bool keepShape = false; // guided: keep each island's transferred shape instead of relaxing it
    bool separateUnmatched = true; // guided: faces with no source counterpart get islands of their own
};

/** Where a target sample lands on the source: triangle + barycentrics, or nothing within range. */
struct Sample
{
    Index triangle = geo::kInvalid;
    glm::dvec3 bary{0.0};
};

/** The source's lookup structure. Depends only on the source mesh and whether sampling is in UV space. */
class Sampler
{
public:
    Sampler(const geo::Mesh& source, bool uvSpace) : mUvSpace(uvSpace)
    {
        if (uvSpace)
        {
            // Sampling in UV space: a flat copy of the source with points at their uvs, one point per corner.
            auto uv = source.cornerAttrs().get<glm::vec2>("uv0");
            std::vector<glm::dvec3> pts;
            std::vector<Index> sizes, idx;
            for (Index f = 0; f < source.faceCapacity(); ++f)
            {
                if (!source.faceValid(f))
                    continue;
                sizes.push_back(source.faceSize(f));
                source.forEachFaceCorner(f, [&](Index c) {
                    idx.push_back(Index(pts.size()));
                    glm::vec2 t = c < uv.size() ? uv[c] : glm::vec2(0.0f);
                    pts.emplace_back(t.x, t.y, 0.0);
                    mUvCornerOf.push_back(c);
                });
            }
            mUvMesh = geo::Mesh::fromPolygons(pts, sizes, idx);
            mBvh.build(mUvMesh);
        }
        else
            mBvh.build(source);
    }

    bool uvSpace() const { return mUvSpace; }

    Sample at(const glm::dvec3& p, const glm::dvec3& dir, const Options& o) const
    {
        Sample s;
        double limit = o.maxDistance > 0.0 ? o.maxDistance : std::numeric_limits<double>::max();
        if (o.search == Search::AlongNormal && glm::length(dir) > 1e-12)
        {
            geo::BvhHit fwd, back;
            bool hf = mBvh.raycast(p, dir, fwd, limit), hb = mBvh.raycast(p, -dir, back, limit);
            if (hf || hb)
            {
                const geo::BvhHit& h = (hf && (!hb || fwd.t <= back.t)) ? fwd : back;
                s.triangle = h.triangle;
                s.bary = h.bary;
                return s;
            }
        }
        geo::BvhClosest c;
        if (mBvh.closestPoint(p, c, limit))
        {
            s.triangle = c.triangle;
            s.bary = c.bary;
        }
        return s;
    }

    /** Source corner ids of a sampled triangle, in the source mesh's numbering. */
    std::array<Index, 3> corners(Index triangle) const
    {
        const Index* c = &mBvh.triangles().corners[size_t(triangle) * 3];
        if (mUvSpace)
            return {mUvCornerOf[mUvMesh.cornerPoint(c[0])], mUvCornerOf[mUvMesh.cornerPoint(c[1])], mUvCornerOf[mUvMesh.cornerPoint(c[2])]};
        return {c[0], c[1], c[2]};
    }

private:
    bool mUvSpace = false;
    geo::Mesh mUvMesh;
    std::vector<Index> mUvCornerOf;
    geo::MeshBvh mBvh;
};

template <class T> T blend(std::span<const T> values, const std::array<Index, 3>& c, const glm::dvec3& w)
{
    return values[c[0]] * float(w.x) + values[c[1]] * float(w.y) + values[c[2]] * float(w.z);
}

/** Copies a corner layer by sampling the source per target corner. */
template <class T>
void transferCornerLayer(const geo::Mesh& src, geo::Mesh& dst, std::string_view name, const std::vector<Sample>& cornerSamples,
                         const Sampler& sampler, std::string_view toName = {})
{
    auto from = src.cornerAttrs().get<T>(name);
    if (from.empty())
        return;
    auto to = dst.cornerAttrs().getOrAdd<T>(toName.empty() ? name : toName);
    tbb::parallel_for(Index(0), dst.cornerCapacity(), [&](Index c) {
        const Sample& s = cornerSamples[c];
        if (s.triangle != geo::kInvalid && dst.cornerValid(c))
            to[c] = blend<T>(from, sampler.corners(s.triangle), s.bary);
    });
}

/** (source set, target set) pairs the options ask for. */
std::vector<std::pair<std::string, std::string>> colorSetPairs(const geo::Mesh& src, const Options& o)
{
    std::vector<std::pair<std::string, std::string>> out;
    if (!o.colors)
        return out;
    if (!o.sourceColorSet.empty())
    {
        if (geo::colors::has(src, o.sourceColorSet))
            out.emplace_back(o.sourceColorSet, o.targetColorSet.empty() ? o.sourceColorSet : o.targetColorSet);
        return out;
    }
    for (const std::string& s : geo::colors::sets(src))
        out.emplace_back(s, s);
    return out;
}

/** Makes sure the target has each set before it is written; a target with no sets takes the source's current one as current. */
void prepareColorSets(const geo::Mesh& src, geo::Mesh& dst, const std::vector<std::pair<std::string, std::string>>& pairs)
{
    bool hadSets = !geo::colors::sets(dst).empty();
    for (const auto& [from, to] : pairs)
    {
        geo::colors::add(dst, to);
        if (geo::colors::isHalf(src, from))
            geo::colors::setHalf(dst, to, true);
    }
    if (!hadSets)
        for (const auto& [from, to] : pairs)
            if (from == geo::colors::current(src))
                geo::colors::setCurrent(dst, to);
}

/** What a guided transfer found, for the command's result. */
struct GuidedStats
{
    uv::AlignResult align;
    uv::GuidedUvResult uvs;
};

void transfer(const geo::Mesh& src, const glm::dmat4& srcWorld, geo::Mesh& dst, const glm::dmat4& dstWorld, const Options& o,
              const Sampler* cached = nullptr, GuidedStats* stats = nullptr)
{
    LILA_ZONE();

    // Target positions expressed in the source's object space.
    glm::dmat4 toSource = o.space == TransferSpace::World || o.space == TransferSpace::Guided ? glm::inverse(srcWorld) * dstWorld : glm::dmat4(1.0);
    GuidedStats guided;
    if (o.space == TransferSpace::Guided)
    {
        // Similar shapes anywhere in the scene: fit the target onto the source, then sample as in world space.
        guided.align = uv::alignShapes(dst, src, toSource);
        toSource = guided.align.transform;
    }
    glm::dmat3 normalToSource = glm::transpose(glm::inverse(glm::dmat3(toSource)));
    glm::dmat3 normalToTarget = glm::inverse(normalToSource);
    auto flip = [&](glm::dvec3 p) {
        if (o.flipAxis >= 0)
            p[o.flipAxis] = -p[o.flipAxis];
        return p;
    };
    const auto colorPairs = colorSetPairs(src, o);
    prepareColorSets(src, dst, colorPairs);

    if (o.space == TransferSpace::Component)
    {
        // Same topology: copy by id. Layers line up as long as both meshes are compacted the same way.
        for (Index p = 0; p < std::min(dst.pointCapacity(), src.pointCapacity()); ++p)
            if (o.positions && dst.pointValid(p) && src.pointValid(p))
                dst.setPosition(p, src.position(p));
        auto copyLayers = [&](geo::Domain d, auto&& wanted) {
            const geo::AttributeSet& from = src.attributes(d);
            geo::AttributeSet& to = dst.attributes(d);
            for (size_t i = 0; i < from.layerCount(); ++i)
            {
                const geo::Attribute& a = from.layer(i);
                if (!wanted(a.name()))
                    continue;
                geo::Attribute& b = to.add(a.name(), a.type());
                for (size_t k = 0; k < std::min(a.size(), b.size()); ++k)
                    std::memcpy(b.raw(k), a.raw(k), a.stride());
            }
        };
        bool mappedUvs = o.uvs && o.sourceUvSet >= 0;
        auto wanted = [&](const std::string& n) {
            return (o.uvs && !mappedUvs && n.rfind("uv", 0) == 0) || (o.normals && n == geo::attr::kNormal) ||
                   std::find(o.attributes.begin(), o.attributes.end(), n) != o.attributes.end();
        };
        copyLayers(geo::Domain::Corner, wanted);
        copyLayers(geo::Domain::Point, wanted);
        if (mappedUvs)
            if (auto from = src.cornerAttrs().get<glm::vec2>(geo::attr::uvName(o.sourceUvSet)); !from.empty())
            {
                auto to = dst.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(o.targetUvSet >= 0 ? o.targetUvSet : o.sourceUvSet));
                std::copy_n(from.begin(), std::min(from.size(), to.size()), to.begin());
            }
        for (const auto& [from, to] : colorPairs)
        {
            auto a = geo::colors::values(src, from);
            auto b = geo::colors::values(dst, to);
            std::copy_n(a.begin(), std::min(a.size(), b.size()), b.begin());
            geo::colors::settle(dst, to);
        }
        return;
    }

    std::optional<Sampler> own;
    if (!cached || cached->uvSpace() != (o.space == TransferSpace::Uv))
        own.emplace(src, o.space == TransferSpace::Uv);
    const Sampler& sampler = own ? *own : *cached;
    auto dstNormals = o.positions ? geo::pointNormals(dst) : std::vector<glm::vec3>{};
    auto dstUv = dst.cornerAttrs().get<glm::vec2>("uv0");

    // Corner samples sit a hair inside their face, so a corner on a UV seam picks the island its face belongs to.
    std::vector<Sample> cornerSamples(dst.cornerCapacity());
    tbb::parallel_for(Index(0), dst.faceCapacity(), [&](Index f) {
        if (!dst.faceValid(f))
            return;
        glm::dvec3 centre = dst.faceCentroid(f);
        glm::dvec3 n = dst.faceNormal(f);
        dst.forEachFaceCorner(f, [&](Index c) {
            glm::dvec3 p;
            if (o.space == TransferSpace::Uv)
            {
                glm::vec2 t = c < dstUv.size() ? dstUv[c] : glm::vec2(0.0f);
                p = glm::dvec3(t.x, t.y, 0.0);
            }
            else
                p = glm::dvec3(toSource * glm::dvec4(glm::mix(dst.position(dst.cornerPoint(c)), centre, 1e-3), 1.0));
            cornerSamples[c] = sampler.at(flip(p), glm::normalize(normalToSource * n), o);
        });
    });

    if (o.uvs && o.space == TransferSpace::Guided)
    {
        // The guide set (the one chosen, or the first) is rebuilt island by island; any other sets are sampled.
        const int from = std::max(o.sourceUvSet, 0);
        uv::GuidedUvOptions g;
        g.sourceSet = geo::attr::uvName(from);
        g.targetSet = geo::attr::uvName(o.targetUvSet >= 0 ? o.targetUvSet : from);
        g.relax = !o.keepShape;
        g.separateUnmatched = o.separateUnmatched;
        glm::dmat4 mirror(1.0);
        if (o.flipAxis >= 0)
            mirror[o.flipAxis][o.flipAxis] = -1.0;
        guided.uvs = uv::guidedUvs(src, dst, mirror * toSource, g);
        if (o.sourceUvSet < 0)
            for (int set = 1; set < 8; ++set)
                transferCornerLayer<glm::vec2>(src, dst, geo::attr::uvName(set), cornerSamples, sampler);
    }
    else if (o.uvs && o.sourceUvSet >= 0)
        transferCornerLayer<glm::vec2>(src, dst, geo::attr::uvName(o.sourceUvSet), cornerSamples, sampler,
                                       geo::attr::uvName(o.targetUvSet >= 0 ? o.targetUvSet : o.sourceUvSet));
    else if (o.uvs)
        for (int set = 0; set < 8; ++set)
            transferCornerLayer<glm::vec2>(src, dst, geo::attr::uvName(set), cornerSamples, sampler);
    for (const auto& [from, to] : colorPairs)
    {
        transferCornerLayer<glm::vec4>(src, dst, geo::colors::layerName(from), cornerSamples, sampler, geo::colors::layerName(to));
        geo::colors::settle(dst, to);
    }
    if (o.normals)
    {
        auto srcNormals = geo::cornerNormals(src);
        auto to = dst.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
        tbb::parallel_for(Index(0), dst.cornerCapacity(), [&](Index c) {
            const Sample& s = cornerSamples[c];
            if (s.triangle == geo::kInvalid || !dst.cornerValid(c))
                return;
            glm::dvec3 n(blend<glm::vec3>(srcNormals, sampler.corners(s.triangle), s.bary));
            if (o.flipAxis >= 0)
                n[o.flipAxis] = -n[o.flipAxis];
            to[c] = glm::vec3(glm::normalize(normalToTarget * n));
        });
    }
    for (const std::string& name : o.attributes)
    {
        if (const geo::Attribute* a = src.cornerAttrs().find(name))
            switch (a->type())
            {
            case geo::AttrType::Float: transferCornerLayer<float>(src, dst, name, cornerSamples, sampler); break;
            case geo::AttrType::Float2: transferCornerLayer<glm::vec2>(src, dst, name, cornerSamples, sampler); break;
            case geo::AttrType::Float3: transferCornerLayer<glm::vec3>(src, dst, name, cornerSamples, sampler); break;
            case geo::AttrType::Float4: transferCornerLayer<glm::vec4>(src, dst, name, cornerSamples, sampler); break;
            default: break;
            }
    }
    if (o.positions)
    {
        // Points sample at themselves; the result goes back into the target's space.
        glm::dmat4 fromSource = glm::inverse(toSource);
        std::vector<glm::dvec3> moved(dst.pointCapacity());
        tbb::parallel_for(Index(0), dst.pointCapacity(), [&](Index p) {
            if (!dst.pointValid(p))
                return;
            glm::dvec3 at = glm::dvec3(toSource * glm::dvec4(dst.position(p), 1.0));
            Sample s = sampler.at(flip(at), glm::normalize(normalToSource * glm::dvec3(dstNormals[p])), o);
            moved[p] = dst.position(p);
            if (s.triangle == geo::kInvalid)
                return;
            auto c = sampler.corners(s.triangle);
            glm::dvec3 q = src.position(src.cornerPoint(c[0])) * s.bary.x + src.position(src.cornerPoint(c[1])) * s.bary.y +
                           src.position(src.cornerPoint(c[2])) * s.bary.z;
            moved[p] = glm::dvec3(fromSource * glm::dvec4(flip(q), 1.0));
        });
        for (Index p = 0; p < dst.pointCapacity(); ++p)
            if (dst.pointValid(p))
                dst.setPosition(p, moved[p]);
    }
    if (stats)
        *stats = guided;
}

TransferSpace spaceFrom(const std::string& s)
{
    return s == "object" ? TransferSpace::Object : s == "component" ? TransferSpace::Component : s == "uv" ? TransferSpace::Uv : s == "guided" ? TransferSpace::Guided : TransferSpace::World;
}

int flipFrom(const std::string& s) { return s == "x" ? 0 : s == "y" ? 1 : s == "z" ? 2 : -1; }

std::vector<std::string> splitNames(const std::string& s)
{
    std::vector<std::string> out;
    std::string part;
    std::stringstream in(s);
    while (std::getline(in, part, ','))
    {
        part.erase(0, part.find_first_not_of(" \t"));
        part.erase(part.find_last_not_of(" \t") + 1);
        if (!part.empty())
            out.push_back(part);
    }
    return out;
}

/**
 * Transfer Attributes as a live modifier: the source mesh and its world matrix are connected in, so editing or moving
 * the source updates this mesh. The source's lookup tree is kept until the source mesh changes.
 */
class TransferModifierNode final : public ModifierNode
{
public:
    void compute(const EvalContext& ctx) override
    {
        ModifierNode::compute(ctx);
        setOutput(type().attrIndex("cacheBuilds"), int64_t(mBuilds));
    }

    geo::Mesh run(const geo::Mesh& in) const
    {
        LILA_ZONE();

        MeshPtr source = as<MeshPtr>(attr("sourceMesh"));
        if (!source)
            throw std::runtime_error("no source mesh: connect one with Use Selected as Source (transferSource)");
        Options o = options();
        if (source != mSource)
        {
            mSource = source;
            mCompacted.reset();
            mSampler.reset();
        }
        if (!mCompacted)
        {
            if (source->hasDeleted())
            {
                auto c = std::make_shared<geo::Mesh>(*source);
                c->compact();
                mCompacted = std::move(c);
            }
            else
                mCompacted = source;
        }
        if (o.space != TransferSpace::Component && (!mSampler || mSampler->uvSpace() != (o.space == TransferSpace::Uv)))
        {
            mSampler = std::make_shared<Sampler>(*mCompacted, o.space == TransferSpace::Uv);
            ++mBuilds;
        }
        geo::Mesh out = in;
        out.compact();
        transfer(*mCompacted, as<glm::dmat4>(attr("sourceMatrix")), out, as<glm::dmat4>(attr("targetMatrix")), o, mSampler.get());
        return out;
    }

private:
    int attr(const char* name) const { return type().attrIndex(name); }

    Options options() const
    {
        Options o;
        o.uvs = as<bool>(attr("uvs"));
        o.normals = as<bool>(attr("normals"));
        o.colors = as<bool>(attr("colors"));
        o.positions = as<bool>(attr("positions"));
        o.attributes = splitNames(as<std::string>(attr("attributes")));
        o.space = TransferSpace(std::clamp<int64_t>(as<int64_t>(attr("sampleSpace")), 0, 4));
        o.search = as<int64_t>(attr("searchMethod")) == 1 ? Search::AlongNormal : Search::Closest;
        o.maxDistance = as<double>(attr("maxDistance"));
        o.flipAxis = int(std::clamp<int64_t>(as<int64_t>(attr("flip")), 0, 3)) - 1;
        o.sourceUvSet = int(std::clamp<int64_t>(as<int64_t>(attr("sourceUvSet")), -1, 7));
        o.targetUvSet = int(std::clamp<int64_t>(as<int64_t>(attr("targetUvSet")), -1, 7));
        o.sourceColorSet = as<std::string>(attr("sourceColorSet"));
        o.targetColorSet = as<std::string>(attr("targetColorSet"));
        o.keepShape = as<bool>(attr("keepShape"));
        o.separateUnmatched = as<bool>(attr("separateUnmatched"));
        return o;
    }

    mutable MeshPtr mSource;
    mutable MeshPtr mCompacted;
    mutable std::shared_ptr<const Sampler> mSampler;
    mutable uint64_t mBuilds = 0;
};

AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, std::vector<std::string> enums = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .enumNames = std::move(enums), .doc = std::move(doc)};
}

AttrDef limitedTransferParam(AttrDef a, std::optional<double> lo, std::optional<double> hi)
{
    a.minValue = lo;
    a.maxValue = hi;
    return a;
}

Node& meshNode(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

int attrOf(const Node& n, const char* name)
{
    int a = n.type().attrIndex(name);
    if (a < 0)
        throw CommandError(n.path() + " has no attribute " + name);
    return a;
}

/**
 * The soft-normal proxy around points lo..hi (object space): an ellipsoid filling the box, or for a dome one centred
 * on the bottom of the box reaching its top, so every point's closest surface faces up and out.
 */
geo::Mesh foliageProxy(const glm::dvec3& lo, const glm::dvec3& hi, bool dome, int up)
{
    geo::Mesh m = geo::makeIcoSphere({.radius = 1.0, .subdivisions = 4});
    glm::dvec3 centre = (lo + hi) * 0.5, radii = glm::max((hi - lo) * 0.5, glm::dvec3(1e-3));
    if (dome)
    {
        centre[up] = lo[up];
        radii[up] = std::max(hi[up] - lo[up], 1e-3);
    }
    // A little larger than the mesh, so closest points sit outside every leaf.
    radii *= 1.05;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, centre + m.position(p) * radii);
    return m;
}

/** Corner normals blended toward the proxy's by mix (0 keeps the mesh's own, 1 takes the proxy's). */
void softFoliageNormals(geo::Mesh& mesh, double mix, bool dome, int up)
{
    LILA_ZONE();

    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(std::numeric_limits<double>::lowest());
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
        {
            lo = glm::min(lo, mesh.position(p));
            hi = glm::max(hi, mesh.position(p));
        }
    if (lo.x > hi.x)
        return;
    std::vector<glm::vec3> own;
    if (auto stored = std::as_const(mesh).cornerAttrs().get<glm::vec3>(geo::attr::kNormal); !stored.empty())
        own.assign(stored.begin(), stored.end());
    else
        own = geo::cornerNormals(mesh);

    Options o;
    o.uvs = false;
    o.normals = true;
    o.space = TransferSpace::Object;
    geo::Mesh proxy = foliageProxy(lo, hi, dome, up);
    transfer(proxy, glm::dmat4(1.0), mesh, glm::dmat4(1.0), o);
    auto normals = mesh.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    const float t = float(std::clamp(mix, 0.0, 1.0));
    for (Index c = 0; c < Index(std::min(normals.size(), own.size())); ++c)
    {
        glm::vec3 n = glm::mix(own[c], normals[c], t);
        normals[c] = glm::length(n) > 1e-6f ? glm::normalize(n) : normals[c];
    }
}

/** Connects source's outMesh and worldMatrix into the modifier and stores the target's current world matrix. */
void connectSource(Graph& g, Node& mod, Node& source)
{
    NodeId target = modifiers::meshOf(g, mod.id());
    if (target == source.id())
        throw CommandError("transferSource: a mesh can't transfer from itself");
    g.connect({source.id(), MeshNode::kOutMesh, -1}, {mod.id(), attrOf(mod, "sourceMesh"), -1}, true);
    g.connect({source.id(), Transform::kWorldMatrix, -1}, {mod.id(), attrOf(mod, "sourceMatrix"), -1}, true);
    if (Node* t = g.node(target))
        g.setValue({mod.id(), attrOf(mod, "targetMatrix"), -1}, t->as<glm::dmat4>(Transform::kWorldMatrix));
}
} // namespace

void registerTransferModifier()
{
    if (types::find("transferAttributesModifier"))
        return;
    NodeType t;
    t.name = "transferAttributesModifier";
    t.icon = "transfer";
    t.doc = "Transfer Attributes kept live: copies UVs, normals, color sets, positions or named layers from a source mesh, so "
            "editing or moving the source updates this mesh without baking. Apply bakes the result.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        param("enabled", PlugType::Bool, true, "Turn the modifier off without removing it."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable,
         .doc = "Apply the modifier when exporting."},
        {.name = "sourceMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput,
         .doc = "The source's outMesh. Set it with Use Selected as Source (transferSource)."},
        {.name = "sourceMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
         .doc = "The source's worldMatrix."},
        {.name = "targetMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "This mesh's world matrix when the source was set (transferSource refreshes it)."},
        param("uvs", PlugType::Bool, true, "Every UV set (or the one sourceUvSet names)."),
        param("normals", PlugType::Bool, false, "Corner normals."),
        param("colors", PlugType::Bool, false, "Vertex color sets (all, or the one sourceColorSet names)."),
        param("positions", PlugType::Bool, false, "Point positions: shrink-wrap this mesh onto the source."),
        {.name = "attributes", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "Other corner layers by name, comma separated."},
        param("sampleSpace", PlugType::Enum, int64_t(0),
              "world, object, component (same topology, by id), uv, or guided (a similar shape anywhere: fitted onto the source, "
              "its UV islands rebuilt on this mesh).",
              kSpaceNames),
        param("searchMethod", PlugType::Enum, int64_t(0), "closest point, or along the normal (falling back to closest).", kSearchNames),
        param("keepShape", PlugType::Bool, false,
              "Guided: keep each island's shape as transferred (trim sheets, hand-laid strips) instead of relaxing it; islands that "
              "fold are still unwrapped."),
        param("separateUnmatched", PlugType::Bool, true,
              "Guided: faces with nothing like them on the source (a cap where the source is open) become islands of their own "
              "beside the layout; off, they join the island next to them."),
        limitedTransferParam(param("maxDistance", PlugType::Float, -1.0, "Ignore matches farther than this, cm; negative is unlimited."), -1.0, {}),
        param("flip", PlugType::Enum, int64_t(0), "Mirror the lookup across the source's x, y or z axis (symmetry).", kFlipNames),
        limitedTransferParam(param("sourceUvSet", PlugType::Int, int64_t(-1), "Transfer only this UV set, 0 to 7; -1 transfers every set."), -1.0, 7.0),
        limitedTransferParam(param("targetUvSet", PlugType::Int, int64_t(-1), "Write the source UV set into this set; -1 keeps its index."), -1.0, 7.0),
        {.name = "sourceColorSet", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "Transfer only this color set; empty transfers every set."},
        {.name = "targetColorSet", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "Write the source color set into this set; empty keeps its name."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "cacheBuilds", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput | AttrHidden,
         .doc = "How many times the source's lookup tree was built."},
    };
    const int out = int(t.ownAttrs.size()) - 2;
    t.create = [out] {
        auto n = std::make_unique<TransferModifierNode>();
        n->outAttr = out;
        n->apply = [](const geo::Mesh& m, const Node& self) { return static_cast<const TransferModifierNode&>(self).run(m); };
        return n;
    };
    types::registerType(std::move(t));
}

void registerTransferCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "transferAttributes",
        .doc = "Copy UVs, normals, color sets, positions or named attributes from the first object to "
               "the others, sampling in world, object, component (same topology) or uv space, by closest point or along normals. "
               "guided is for similar but different pieces anywhere in the scene: each target is fitted onto the source and its UVs are "
               "rebuilt from the source's islands (seams carried over, each island unwrapped and fitted onto its source island). "
               "live=True adds a transferAttributes modifier to each target instead of baking, so the targets follow the source.",
        .positionalDoc = "source targets...",
        .flags = {
            {"uvs", "", T::Bool, true, "Every UV set."},
            {"normals", "", T::Bool, false, "Corner normals."},
            {"colors", "", T::Bool, false, "Vertex color sets."},
            {"positions", "", T::Bool, false, "Point positions (shrink-wrap the target onto the source)."},
            {"attributes", "", T::List, nullptr, "Other corner layers by name."},
            {"skinWeights", "", T::Bool, false, "Skin weights, by closest point (point index in component space). Both meshes must be skinned."},
            {"sampleSpace", "", T::String, "world", "world, object, component, uv or guided.", {"world", "object", "component", "uv", "guided"}},
            {"searchMethod", "", T::String, "closest", "closest or normal.", {"closest", "normal"}},
            {"keepShape", "", T::Bool, false,
             "Guided: keep each island's shape as transferred instead of relaxing it (trim sheets); islands that fold are still unwrapped."},
            {"separateUnmatched", "", T::Bool, true,
             "Guided: faces with nothing like them on the source become islands of their own beside the layout; off, they join the "
             "island next to them."},
            {"maxDistance", "", T::Float, -1.0, "Ignore matches farther than this (cm); negative is unlimited.", {}, {}, -1.0},
            {"flip", "", T::String, "none", "Mirror the lookup across the source's x, y or z axis.", {"none", "x", "y", "z"}},
            {"sourceUvSet", "", T::Int, -1, "Transfer only this UV set (0-7); -1 transfers every set.", {}, {}, -1.0, 7.0},
            {"targetUvSet", "", T::Int, -1, "Write the source UV set into this target set; -1 keeps its index.", {}, {}, -1.0, 7.0},
            {"sourceColorSet", "", T::String, "", "Transfer only this color set; empty transfers every set."},
            {"targetColorSet", "", T::String, "", "Write the source color set into this target set; empty keeps its name."},
            {"live", "", T::Bool, false, "Add a live transfer modifier (follows the source) instead of baking. Returns the modifiers."},
        },
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("transferAttributes: give a source and at least one target");
            Graph& g = Graph::current();
            g.evaluate();
            Options o;
            o.uvs = a.get<bool>("uvs", true);
            o.normals = a.get<bool>("normals");
            o.colors = a.get<bool>("colors");
            o.positions = a.get<bool>("positions");
            if (a.has("attributes"))
                o.attributes = a.flags["attributes"].get<std::vector<std::string>>();
            std::string space = a.get<std::string>("sampleSpace", "world");
            o.space = spaceFrom(space);
            o.search = a.get<std::string>("searchMethod", "closest") == "normal" ? Search::AlongNormal : Search::Closest;
            o.maxDistance = a.get<double>("maxDistance", -1.0);
            std::string flipAxis = a.get<std::string>("flip", "none");
            o.flipAxis = flipFrom(flipAxis);
            o.sourceUvSet = std::clamp(a.get<int>("sourceUvSet", -1), -1, 7);
            o.targetUvSet = std::clamp(a.get<int>("targetUvSet", -1), -1, 7);
            o.sourceColorSet = a.get<std::string>("sourceColorSet", "");
            o.targetColorSet = a.get<std::string>("targetColorSet", "");
            o.keepShape = a.get<bool>("keepShape");
            o.separateUnmatched = a.get<bool>("separateUnmatched", true);
            bool skinWeights = a.get<bool>("skinWeights");

            Node& src = meshNode(g, objs.front(), "transferAttributes");
            if (a.get<bool>("live"))
            {
                std::string attrs;
                for (const std::string& n : o.attributes)
                    attrs += (attrs.empty() ? "" : ",") + n;
                json params = {{"uvs", o.uvs},
                               {"normals", o.normals},
                               {"colors", o.colors},
                               {"positions", o.positions},
                               {"attributes", attrs},
                               {"sampleSpace", int(o.space)},
                               {"searchMethod", int(o.search)},
                               {"maxDistance", o.maxDistance},
                               {"flip", o.flipAxis + 1},
                               {"sourceUvSet", o.sourceUvSet},
                               {"targetUvSet", o.targetUvSet},
                               {"sourceColorSet", o.sourceColorSet},
                               {"targetColorSet", o.targetColorSet},
                               {"keepShape", o.keepShape},
                               {"separateUnmatched", o.separateUnmatched}};
                UndoChunk chunk("Transfer Attributes (Live)");
                json out = json::array();
                for (size_t i = 1; i < objs.size(); ++i)
                {
                    meshNode(g, objs[i], "transferAttributes");
                    std::string mod = ops::execute("addModifier", {{"args", {objs[i]}}, {"kind", "transferAttributes"}, {"params", params}}).get<std::string>();
                    connectSource(g, *g.find(mod), src);
                    out.push_back(mod);
                }
                return out;
            }

            MeshPtr srcMesh = src.as<MeshPtr>(MeshNode::kOutMesh);
            if (!srcMesh)
                throw CommandError("transferAttributes: source has no mesh");
            geo::Mesh source = *srcMesh;
            source.compact();
            glm::dmat4 srcWorld = src.as<glm::dmat4>(Transform::kWorldMatrix);

            UndoChunk chunk("Transfer Attributes");
            json report = json::array();
            for (size_t i = 1; i < objs.size(); ++i)
            {
                Node& dstNode = meshNode(g, objs[i], "transferAttributes");
                PlugRef base = modifiers::editPlug(g, dstNode.id());
                g.evaluate();
                MeshPtr current = std::get<MeshPtr>(g.storedValue(base));
                if (!current)
                    continue;
                geo::Mesh target = *current;
                GuidedStats stats;
                transfer(source, srcWorld, target, dstNode.as<glm::dmat4>(Transform::kWorldMatrix), o, nullptr, &stats);
                if (o.space == TransferSpace::Guided)
                    report.push_back({{"target", dstNode.path()},
                                      {"fitError", stats.align.residual},
                                      {"reoriented", stats.align.searched},
                                      {"mirrored", stats.align.mirrored},
                                      {"islands", stats.uvs.islands},
                                      {"relaxed", stats.uvs.relaxed},
                                      {"repaired", stats.uvs.repaired},
                                      {"unwrapped", stats.uvs.unfolded},
                                      {"unmatched", stats.uvs.unmatched},
                                      {"seams", stats.uvs.seams}});
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(target))));
                if (skinWeights)
                    ops::execute("copySkinWeights", {{"args", {objs.front(), objs[i]}},
                                                      {"surfaceAssociation", o.space == TransferSpace::Component ? "pointIndex" : "closestPoint"}});
            }
            return report.empty() ? json(nullptr) : report;
        },
    });

    ops::registerCommand({
        .name = "softFoliageNormals",
        .doc = "Soft Foliage Normals: transfer normals from a generated dome or sphere around each mesh onto it, mixed with the "
               "mesh's own, so leaf clumps shade as one soft volume instead of many cards.",
        .positionalDoc = "meshes...",
        .flags = {
            {"mix", "", T::Float, 0.8, "0 keeps the mesh's own normals, 1 takes the proxy's.", {}, {}, 0.0, 1.0},
            {"proxy", "", T::String, "dome", "dome (centred on the bottom, normals face up and out) or sphere.", {"dome", "sphere"}},
            {"up", "", T::String, "y", "The meshes' up axis, for the dome.", {"y", "z"}},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            std::vector<NodeId> meshes;
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n)
                    throw CommandError("softFoliageNormals: no object " + name);
                if (n->type().isA("mesh"))
                    meshes.push_back(n->id());
                for (NodeId d : g.descendants(n->id()))
                    if (g.node(d)->type().isA("mesh"))
                        meshes.push_back(d);
            }
            if (meshes.empty())
                throw CommandError("softFoliageNormals: select meshes");
            const double mix = a.get<double>("mix", 0.8);
            const bool dome = a.get<std::string>("proxy", "dome") != "sphere";
            const int up = a.get<std::string>("up", "y") == "z" ? 2 : 1;
            UndoChunk chunk("Soft Foliage Normals");
            json out = json::array();
            for (NodeId id : meshes)
            {
                PlugRef base = modifiers::editPlug(g, id);
                g.evaluate();
                MeshPtr current = std::get<MeshPtr>(g.storedValue(base));
                if (!current)
                    continue;
                geo::Mesh mesh = *current;
                softFoliageNormals(mesh, mix, dome, up);
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
                out.push_back(g.node(id)->path());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "transferSource",
        .doc = "Set the source mesh of transferAttributes modifiers: connects its mesh and world matrix, and refreshes the target's "
               "stored world matrix (run it again after moving the target).",
        .positionalDoc = "modifiers..., source",
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("transferSource: give the modifiers, then the source mesh");
            Graph& g = Graph::current();
            Node& source = meshNode(g, objs.back(), "transferSource");
            for (size_t i = 0; i + 1 < objs.size(); ++i)
            {
                Node* mod = g.find(objs[i]);
                if (!mod || mod->type().name != "transferAttributesModifier")
                    throw CommandError("transferSource: " + objs[i] + " is not a transferAttributes modifier");
                connectSource(g, *mod, source);
            }
            return nullptr;
        },
    });
}
} // namespace lila::scene
