#include "scene/MeshCache.h"

#include "core/PackedArray.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/RigUtil.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lila::scene
{
using namespace rigutil;

namespace
{
/** A json number array as T: packed (any dtype) or plain, flat or in rows. */
template <class T> void readNumbers(const json& j, std::vector<T>& out)
{
    out.clear();
    if (packed::is(j))
    {
        const auto& bin = j.get_binary();
        const size_t n = packed::count(j);
        if (packed::dtype(j) == packed::dtypeOf<T>())
        {
            out.resize(n);
            std::memcpy(out.data(), bin.data(), n * sizeof(T));
            return;
        }
        out.reserve(n);
        for (size_t i = 0; i < n; ++i)
            out.push_back(packed::element(packed::dtype(j), bin.data(), i).template get<T>());
        return;
    }
    if (!j.is_array())
        return;
    for (const json& x : j)
        if (x.is_array())
            for (const json& y : x)
                out.push_back(y.get<T>());
        else
            out.push_back(x.get<T>());
}

enum Param
{
    kSamples = DeformerNode::kFirstParam,
    kTimeOffset,
    kSpeed,
    kInterpolate,
    kOutOfRange,
};

class MeshCacheNode : public DeformerNode
{
public:
    void compute(const EvalContext& ctx) override
    {
        mTime = ctx.time;
        DeformerNode::compute(ctx);
    }

    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        const auto* s = dynamic_cast<const MeshCacheSamples*>(as<DataPtr>(kSamples).get());
        if (!s || s->sampleCount() == 0)
            throw std::runtime_error("the cache holds no samples");
        if (s->pointCount != points.size())
            throw std::runtime_error("the cache has " + std::to_string(s->pointCount) + " points and the mesh " + std::to_string(points.size()));
        const double t = (mTime - as<double>(kTimeOffset)) * as<double>(kSpeed);
        meshCache::pointsAt(*s, t, as<bool>(kInterpolate), as<int64_t>(kOutOfRange) == 1, points);
    }

private:
    double mTime = 0.0;
};

AttrDef cacheAttr(std::string name, PlugType type, PlugValue def, uint32_t flags, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = flags, .doc = std::move(doc)};
}

std::vector<NodeId> meshArgs(Graph& g, const Args& a, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& name : a.objects())
    {
        Node& n = nodeNamed(name, cmd);
        if (!n.type().isA("mesh") || n.type().isA("lattice"))
            throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
        out.push_back(n.id());
    }
    if (out.empty())
        for (NodeId id : g.selection())
            if (g.node(id)->type().isA("mesh"))
                out.push_back(id);
    if (out.empty())
        throw CommandError(std::string(cmd) + ": give or select a mesh");
    return out;
}
} // namespace

json MeshCacheSamples::toJson() const
{
    return {{"kind", kind()},
            {"pointCount", pointCount},
            {"times", packed::make<double>(times)},
            {"points", packed::make<float>(points, 3)}};
}

DataPtr MeshCacheSamples::fromJson(const json& j)
{
    auto s = std::make_shared<MeshCacheSamples>();
    s->pointCount = j.value("pointCount", 0u);
    if (j.contains("times"))
        readNumbers(j["times"], s->times);
    if (j.contains("points"))
        readNumbers(j["points"], s->points);
    s->times.resize(s->sampleCount());
    s->points.resize(s->times.size() * size_t(s->pointCount) * 3);
    return s;
}

void MeshCacheSamples::add(double seconds, std::span<const glm::dvec3> pts)
{
    if (times.empty())
        pointCount = uint32_t(pts.size());
    if (pts.size() != pointCount)
        throw std::invalid_argument("a mesh cache sample needs " + std::to_string(pointCount) + " points, not " + std::to_string(pts.size()));
    times.push_back(seconds);
    points.reserve(points.size() + pts.size() * 3);
    for (const glm::dvec3& p : pts)
        points.insert(points.end(), {float(p.x), float(p.y), float(p.z)});
}

namespace meshCache
{
NodeId find(Graph& g, NodeId mesh)
{
    for (NodeId id : modifiers::stack(g, mesh))
        if (g.node(id)->type().name == kType)
            return id;
    return kNoNode;
}

std::shared_ptr<const MeshCacheSamples> samples(Graph& g, NodeId cache)
{
    const Node* n = g.node(cache);
    if (!n || n->type().name != kType)
        return nullptr;
    return std::dynamic_pointer_cast<const MeshCacheSamples>(std::get<DataPtr>(g.storedValue({cache, kSamples, -1})));
}

NodeId create(Graph& g, NodeId mesh, std::shared_ptr<const MeshCacheSamples> s, const std::string& name)
{
    const NodeId id = deformers::addDeformer(g, kType, {mesh}, name).front();
    setSamples(g, id, std::move(s));
    return id;
}

void setSamples(Graph& g, NodeId cache, std::shared_ptr<const MeshCacheSamples> s)
{
    g.setValue({cache, kSamples, -1}, DataPtr(std::move(s)));
}

void pointsAt(const MeshCacheSamples& s, double t, bool interpolate, bool loop, std::vector<glm::dvec3>& out)
{
    const size_t n = s.sampleCount();
    out.resize(s.pointCount);
    if (n == 0)
        return;
    const double first = s.times.front(), last = s.times[n - 1];
    if (loop && last > first)
    {
        // The last sample stands for the first again, so a loop of samples 0..n-1 repeats every last - first.
        t = first + std::fmod(t - first, last - first);
        if (t < first)
            t += last - first;
    }
    t = std::clamp(t, first, last);
    const size_t hi = size_t(std::upper_bound(s.times.begin(), s.times.begin() + ptrdiff_t(n), t) - s.times.begin());
    const size_t a = hi == 0 ? 0 : hi - 1, b = std::min(hi, n - 1);
    const double span = s.times[b] - s.times[a];
    const double f = interpolate && b != a && span > 0.0 ? (t - s.times[a]) / span : 0.0;
    const std::span<const float> pa = s.sample(a), pb = s.sample(b);
    for (size_t p = 0; p < out.size(); ++p)
    {
        const glm::dvec3 va(pa[p * 3], pa[p * 3 + 1], pa[p * 3 + 2]);
        out[p] = f == 0.0 ? va : glm::mix(va, glm::dvec3(pb[p * 3], pb[p * 3 + 1], pb[p * 3 + 2]), f);
    }
}

void registerTypes()
{
    if (types::find(kType))
        return;
    data::registerKind("meshCacheSamples", &MeshCacheSamples::fromJson);
    deformers::DeformerSpec spec{
        kType, "movie",
        "Plays back a point cache: every point's position per frame, read from USD time samples or recorded with "
        "bakeMeshCache, interpolated between samples. Deformers above it move the cached motion.",
        {cacheAttr("samples", PlugType::Data, DataPtr(), AttrInput | AttrStorable | AttrHidden, "The cached positions, object space."),
         cacheAttr("timeOffset", PlugType::Float, 0.0, AttrInput | AttrStorable | AttrKeyable,
               "Seconds the cache starts later: cache time is (scene time - timeOffset) * speed."),
         cacheAttr("speed", PlugType::Float, 1.0, AttrInput | AttrStorable | AttrKeyable, "Playback rate of the cache; 2 plays twice as fast."),
         cacheAttr("interpolate", PlugType::Bool, true, AttrInput | AttrStorable,
               "Blend linearly between samples; off holds each sample until the next."),
         {.name = "outOfRange", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
          .enumNames = {"hold", "loop"}, .doc = "Before the first or after the last sample: hold that sample, or loop the cache."}},
        [] { return std::make_unique<MeshCacheNode>(); }};
    spec.timeDependent = true;
    deformers::registerDeformer(std::move(spec));
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "bakeMeshCache",
        .doc = "Record each mesh's points over a frame range into a meshCache deformer on top of its stack, which then plays "
               "them back as they were (turn it off to see the live stack again). Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"start", "", T::Int, nullptr, "First frame; the playback start by default."},
                  {"end", "", T::Int, nullptr, "Last frame; the playback end by default."},
                  {"step", "", T::Int, 1, "Frames between samples."},
                  {"name", "", T::String, nullptr, "Name for the deformers."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<NodeId> meshes = meshArgs(g, a, "bakeMeshCache");
            const anim::Playback& pb = anim::playback();
            const int start = a.has("start") ? a.get<int>("start") : int(std::lround(anim::frameOf(pb.minTime)));
            const int end = a.has("end") ? a.get<int>("end") : int(std::lround(anim::frameOf(pb.maxTime)));
            const int step = std::max(1, a.get<int>("step"));
            if (end < start)
                throw CommandError("bakeMeshCache: end is before start");
            std::vector<std::shared_ptr<MeshCacheSamples>> recorded(meshes.size());
            for (auto& r : recorded)
                r = std::make_shared<MeshCacheSamples>();
            const double saved = g.time();
            try
            {
                for (int f = start; f <= end; f += step)
                {
                    const double t = anim::secondsOf(double(f));
                    g.setTime(t);
                    g.evaluate();
                    for (size_t k = 0; k < meshes.size(); ++k)
                    {
                        const MeshPtr m = g.node(meshes[k])->as<MeshPtr>(MeshNode::kOutMesh);
                        if (!m)
                            throw CommandError("bakeMeshCache: " + g.node(meshes[k])->name() + " has no geometry");
                        if (!recorded[k]->times.empty() && m->pointCapacity() != recorded[k]->pointCount)
                            throw CommandError("bakeMeshCache: " + g.node(meshes[k])->name() + " changes its point count over the range");
                        recorded[k]->add(t, m->positions());
                    }
                }
            }
            catch (...)
            {
                g.setTime(saved);
                throw;
            }
            g.setTime(saved);
            json out = json::array();
            const std::string name = a.has("name") ? a.get<std::string>("name") : "MeshCache";
            for (size_t k = 0; k < meshes.size(); ++k)
                out.push_back(g.node(guarded([&] { return meshCache::create(g, meshes[k], recorded[k], name); }))->name());
            return out;
        },
    });

    ops::registerCommand({
        .name = "meshCacheInfo",
        .doc = "A meshCache's samples: {points, samples, start, end} (seconds). Give the deformer or a mesh with one.",
        .positionalDoc = "meshCache or mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<std::string> objs = a.objects();
            if (objs.size() != 1)
                throw CommandError("meshCacheInfo: give one meshCache or mesh");
            Node& n = nodeNamed(objs.front(), "meshCacheInfo");
            const NodeId cache = n.type().name == kType ? n.id() : n.type().isA("mesh") ? find(g, n.id()) : kNoNode;
            if (cache == kNoNode)
                throw CommandError("meshCacheInfo: " + n.name() + " is not a meshCache and has none");
            const auto s = samples(g, cache);
            const size_t count = s ? s->sampleCount() : 0;
            return {{"name", g.node(cache)->name()},
                    {"points", s ? s->pointCount : 0},
                    {"samples", count},
                    {"start", count ? json(s->times.front()) : json(nullptr)},
                    {"end", count ? json(s->times[count - 1]) : json(nullptr)}};
        },
    });
}
} // namespace meshCache
} // namespace lila::scene
