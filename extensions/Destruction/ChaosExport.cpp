// exportChaos: a destruction's pieces and their motion as a Lila Geometry Collection (.lilagc.json + .lilagc.bin), which
// LilaLink turns into a Chaos Geometry Collection and a Chaos cache: the alternative to rigid vertex animation when a game
// should hand the pieces over to live Chaos.

#include "Destruction.h"
#include "PieceTransform.h"
#include "RigidState.h"

#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Sequences.h>
#include <lila/Simulation.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <format>
#include <filesystem>
#include <fstream>
#include <map>

using namespace lila;

namespace destruction
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;

/** Little-endian arrays appended one after another, each remembered by name with its byte offset and element count. */
struct Bin
{
    std::vector<uint8_t> bytes;
    json buffers = json::object();

    template <class T> void add(const std::string& name, const std::vector<T>& values, size_t count)
    {
        while (bytes.size() % 4)
            bytes.push_back(0);
        buffers[name] = {{"offset", bytes.size()}, {"count", count}};
        const auto* p = reinterpret_cast<const uint8_t*>(values.data());
        bytes.insert(bytes.end(), p, p + values.size() * sizeof(T));
    }
};

/** A rigid transform in Unreal's space from one in Lila's: the basis conjugates the rotation, so it stays a rotation. */
struct UeTransform
{
    glm::dvec3 position{0.0};
    glm::dquat rotation{1, 0, 0, 0};
};

UeTransform toUnreal(const glm::dmat3& basis, const glm::dvec3& p, const glm::dquat& q)
{
    const glm::dmat3 r = basis * glm::mat3_cast(q) * glm::transpose(basis);
    return {basis * p, glm::normalize(glm::quat_cast(r))};
}

json arr(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

/** The modifier running a mesh's destruction simulation (or the node itself when it is one). */
Node* simulationModifier(Graph& g, Node& target)
{
    if (scene::sim::isSimulation(target))
        return &target;
    for (scene::NodeId m : scene::modifiers::stack(g, target.id()))
        if (Node* mod = g.node(m); mod && scene::sim::isSimulation(*mod))
            return mod;
    return nullptr;
}

/** Breaks as exportBreaks finds them, positioned at their rest anchors (world space). */
json breaksOf(const StateView& end, const gn::GeometryPtr& links, const glm::dmat4& world, const glm::dmat3& basis)
{
    json out = json::array();
    if (!links->curves)
        return out;
    const gn::CurveSet& c = *links->curves;
    std::span<const int32_t> pa = c.curveAttrs.get<int32_t>("pieceA"), pb = c.curveAttrs.get<int32_t>("pieceB");
    size_t link = 0;
    for (size_t k = 0; k < c.numCurves() && link < end.header().links; ++k)
    {
        if (c.curveSize(k) == 0)
            continue;
        const LinkRecord r = end.link(link++);
        if (r.brokenStep < 0)
            continue;
        const glm::dvec3 at = basis * glm::dvec3(world * glm::dvec4(c.curvePoints(k)[0], 1.0));
        out.push_back({{"frame", end.frameOfStep(r.brokenStep)}, {"position", arr(at)}, {"impulse", r.impulse},
                       {"pieceA", k < pa.size() ? pa[k] : -1}, {"pieceB", k < pb.size() ? pb[k] : -1}});
    }
    std::stable_sort(out.begin(), out.end(), [](const json& a, const json& b) { return a["frame"].get<int64_t>() < b["frame"].get<int64_t>(); });
    return out;
}

/** Folder for exports: absolute as given, else under the workspace. */
std::filesystem::path outputFolder(const std::string& folder)
{
    std::filesystem::path p(std::u8string(folder.begin(), folder.end()));
    if (p.is_relative())
        p = (Workspace::get().isOpen() ? Workspace::get().root() : std::filesystem::current_path()) / p;
    return p.lexically_normal();
}
} // namespace

void registerChaosExport(ext::Context& ctx)
{
    using AT = ArgType;
    ctx.registerCommand({
        .name = "exportChaos",
        .doc = "Write a destruction as a Chaos Geometry Collection for Unreal: its pieces (with their inside faces marked), and "
               "every frame of their motion for a Chaos cache, so Unreal plays it back and a game can hand the pieces over to live "
               "Chaos. The alternative to rigid vertex animation when the pieces must become physical again. Writes "
               "<folder>/<name>.lilagc.json and .lilagc.bin in Unreal's space; with send, LilaLink makes the Geometry Collection "
               "and the cache at contentPath. Returns {json, bin, pieces, frames, breaks, assets}.",
        .positionalDoc = "destruction mesh or its modifier (default the selection)",
        .flags = {{"folder", "fo", AT::String, "destruction", "Output folder; relative paths are under the workspace."},
                  {"name", "n", AT::String, nullptr, "File name. Default: the mesh's."},
                  {"start", "st", AT::Int, nullptr, "First frame. Default: the start of the playback range."},
                  {"end", "e", AT::Int, nullptr, "Last frame. Default: the end of the playback range."},
                  {"animation", "a", AT::Bool, true, "Write the frames for a Chaos cache; False writes only the pieces."},
                  {"profile", "ax", AT::String, "ue5", "Axis profile: ue5 or ue6.", {"ue5", "ue6"}},
                  {"send", "s", AT::Bool, false, "Make the Geometry Collection in the connected Unreal editor."},
                  {"contentPath", "cp", AT::String, nullptr, "With send: where the Geometry Collection goes, like /Game/Destruction/GC_Wall."},
                  {"place", "", AT::Bool, false, "With send: put it in the open level with a Chaos Cache Manager playing it."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("exportChaos: give one destruction");
            Node* target = g.find(names[0]);
            if (!target)
                throw CommandError("exportChaos: no node named '" + names[0] + "'");
            g.evaluate();
            Node* mod = simulationModifier(g, *target);
            if (!mod)
                throw CommandError("exportChaos: " + target->path() + " has no destruction simulation (run breakApart first)");
            const scene::NodeId meshId = scene::modifiers::meshOf(g, mod->id()) != scene::kNoNode ? scene::modifiers::meshOf(g, mod->id()) : target->id();
            Node* mesh = g.node(meshId);
            const glm::dmat4 world = mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            const glm::dmat3 basis = seq::unrealBasis(a.get<std::string>("profile", "ue5"));
            const bool mirrored = glm::determinant(basis) < 0.0;

            std::shared_ptr<const gn::SimInputs> in = gn::simulationInputs(mod->id());
            if (!in || !in->geometry("pieces")->instances)
                throw CommandError("exportChaos: the destruction has no pieces yet");
            const gn::Instances& inst = *in->geometry("pieces")->instances;
            const size_t count = inst.size();

            const int64_t first = a.has("start") ? a.get<int64_t>("start") : scene::sim::frameOf(scene::anim::playback().minTime);
            const int64_t last = std::max(first, a.has("end") ? a.get<int64_t>("end") : scene::sim::frameOf(scene::anim::playback().maxTime));
            const bool animated = a.get<bool>("animation", true);

            // Pieces: geometry in Unreal's space about each piece's own origin, scale baked in.
            std::vector<std::string> materials;
            std::map<std::string, int32_t> slotOf;
            std::vector<float> positions, normals, uvs;
            std::vector<int32_t> indices, materialIds;
            std::vector<uint8_t> internal;
            std::vector<glm::dvec3> scales(count, glm::dvec3(1.0));
            json pieces = json::array();
            for (size_t k = 0; k < count; ++k)
            {
                const Decomposed rest = decompose(world * inst.transforms[k]);
                scales[k] = rest.scale;
                const int r = inst.reference[k];
                std::shared_ptr<const geo::Mesh> m;
                if (r >= 0 && size_t(r) < inst.references.size() && inst.references[size_t(r)])
                    m = gn::realize(*inst.references[size_t(r)]).mesh;
                const size_t v0 = positions.size() / 3, t0 = indices.size() / 3;
                if (m)
                {
                    std::vector<glm::vec3> fallback;
                    std::span<const glm::vec3> cornerN = m->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
                    if (cornerN.empty())
                    {
                        fallback = geo::cornerNormals(*m);
                        cornerN = fallback;
                    }
                    std::span<const glm::vec2> uv = m->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
                    std::span<const int32_t> mat = m->faceAttrs().get<int32_t>("material");
                    std::span<const int32_t> inside = m->faceAttrs().get<int32_t>("inside");
                    const std::vector<std::string>& slotNames = m->materialNames();
                    std::map<std::tuple<geo::Index, float, float, float, float, float>, int32_t> welded;
                    const glm::dmat3 normalBasis = basis * glm::inverse(glm::dmat3(glm::scale(glm::dmat4(1.0), rest.scale)));
                    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
                    {
                        if (!m->faceValid(f))
                            continue;
                        const int32_t local = f < mat.size() ? mat[f] : 0;
                        const std::string slot = local >= 0 && size_t(local) < slotNames.size() ? slotNames[size_t(local)] : "Default";
                        auto [it, fresh] = slotOf.try_emplace(slot, int32_t(materials.size()));
                        if (fresh)
                            materials.push_back(slot);
                        const std::vector<geo::Index> corners = m->faceCorners(f);
                        std::vector<int32_t> ids;
                        for (geo::Index c : corners)
                        {
                            const geo::Index p = m->cornerPoint(c);
                            const glm::vec3 n = c < cornerN.size() ? cornerN[c] : glm::vec3(0, 1, 0);
                            const glm::vec2 t = c < uv.size() ? uv[c] : glm::vec2(0.0f);
                            auto [w, made] = welded.try_emplace({p, n.x, n.y, n.z, t.x, t.y}, int32_t(positions.size() / 3 - v0));
                            if (made)
                            {
                                const glm::dvec3 pos = basis * (m->position(p) * rest.scale);
                                const glm::dvec3 nrm = glm::normalize(normalBasis * glm::dvec3(n));
                                positions.insert(positions.end(), {float(pos.x), float(pos.y), float(pos.z)});
                                normals.insert(normals.end(), {float(nrm.x), float(nrm.y), float(nrm.z)});
                                // Unreal's V runs down the texture.
                                uvs.insert(uvs.end(), {t.x, 1.0f - t.y});
                            }
                            ids.push_back(w->second);
                        }
                        for (size_t c = 1; c + 1 < ids.size(); ++c)
                        {
                            // A mirroring basis turns the winding inside out; swapping two corners puts the outside back.
                            const int32_t b = mirrored ? ids[c + 1] : ids[c], d = mirrored ? ids[c] : ids[c + 1];
                            indices.insert(indices.end(), {ids[0], b, d});
                            materialIds.push_back(it->second);
                            internal.push_back(f < inside.size() && inside[f] != 0 ? 1 : 0);
                        }
                    }
                }
                const UeTransform t = toUnreal(basis, rest.position, rest.rotation);
                pieces.push_back({{"name", std::format("{}_{:03}", mesh->name(), k)},
                                  {"position", arr(t.position)},
                                  {"rotation", json::array({t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w})},
                                  {"vertexStart", v0},
                                  {"vertexCount", positions.size() / 3 - v0},
                                  {"triangleStart", t0},
                                  {"triangleCount", indices.size() / 3 - t0},
                                  {"releaseFrame", -1}});
            }

            // Motion: every frame's piece transforms, and the first frame each piece leaves its rest.
            std::vector<float> transforms;
            json breaks = json::array();
            int64_t frames = 0;
            if (animated)
            {
                std::vector<int64_t> release(count, -1);
                for (int64_t f = first; f <= last; ++f, ++frames)
                {
                    const std::vector<uint8_t> bytes = gn::simulationState(g, mod->id(), f);
                    const StateView v(bytes);
                    const bool ok = v.valid() && v.header().bodies == count;
                    for (size_t k = 0; k < count; ++k)
                    {
                        const glm::dmat4 m = ok ? world * compose(v.position(k), v.rotation(k), glm::dvec3(1.0)) : world * inst.transforms[k];
                        const Decomposed d = decompose(m);
                        const UeTransform t = toUnreal(basis, d.position, d.rotation);
                        transforms.insert(transforms.end(), {float(t.position.x), float(t.position.y), float(t.position.z), float(t.rotation.x),
                                                             float(t.rotation.y), float(t.rotation.z), float(t.rotation.w)});
                        const Decomposed rest = decompose(world * inst.transforms[k]);
                        if (release[k] < 0 && (glm::length(d.position - rest.position) > 0.5 ||
                                               std::abs(glm::dot(d.rotation, rest.rotation)) < std::cos(glm::radians(0.25))))
                            release[k] = f;
                    }
                    if (f == last && ok)
                        breaks = breaksOf(v, in->geometry("links"), world, basis);
                }
                for (size_t k = 0; k < count; ++k)
                    pieces[k]["releaseFrame"] = release[k];
            }

            Bin bin;
            bin.add("positions", positions, positions.size() / 3);
            bin.add("normals", normals, normals.size() / 3);
            bin.add("uvs", uvs, uvs.size() / 2);
            bin.add("indices", indices, indices.size());
            bin.add("materialIds", materialIds, materialIds.size());
            bin.add("internal", internal, internal.size());
            bin.add("transforms", transforms, transforms.size() / 7);

            const std::string name = a.get<std::string>("name", mesh->name());
            const std::filesystem::path folder = outputFolder(a.get<std::string>("folder", "destruction"));
            std::filesystem::create_directories(folder);
            const std::filesystem::path jsonPath = folder / (name + ".lilagc.json"), binPath = folder / (name + ".lilagc.bin");
            json manifest = {{"format", "lila-geometry-collection"}, {"version", 1},        {"name", name},
                             {"fps", scene::sim::framesPerSecond()},  {"startFrame", first}, {"frameCount", frames},
                             {"materials", materials},                {"pieces", pieces},    {"bin", binPath.filename().string()},
                             {"buffers", bin.buffers},                {"breaks", breaks}};
            {
                std::ofstream out(binPath, std::ios::binary);
                if (!out)
                    throw CommandError("exportChaos: can't write " + binPath.string());
                out.write(reinterpret_cast<const char*>(bin.bytes.data()), std::streamsize(bin.bytes.size()));
                std::ofstream js(jsonPath, std::ios::binary);
                if (!js)
                    throw CommandError("exportChaos: can't write " + jsonPath.string());
                js << manifest.dump(1);
            }

            json result = {{"json", jsonPath.string()}, {"bin", binPath.string()}, {"pieces", count}, {"frames", frames},
                           {"breaks", breaks.size()}, {"assets", json::array()}};
            if (a.get<bool>("send", false))
            {
                if (!a.has("contentPath"))
                    throw CommandError("exportChaos: send needs contentPath, like /Game/Destruction/GC_" + name);
                std::shared_ptr<link::RpcClient> client = link::Link::get().client();
                if (!client)
                    throw CommandError("exportChaos: not connected to Unreal (run unrealConnect)");
                json params = {{"file", std::filesystem::absolute(jsonPath).string()},
                               {"contentPath", a.get<std::string>("contentPath")},
                               {"cache", animated},
                               {"place", a.get<bool>("place", false)},
                               {"materials", link::materialParams(g, {meshId})}};
                link::RpcResult r = client->call("importGeometryCollection", params, link::importCallOptions());
                if (!r.ok)
                    throw CommandError("exportChaos: " + r.error);
                result["assets"] = r.result.value("assets", json::array());
                result["warnings"] = r.result.value("warnings", json::array());
            }
            return result;
        },
        .examples = {"exportChaos('Wall', folder='Export/Chaos')", "exportChaos('Wall', send=True, contentPath='/Game/Destruction/GC_Wall', place=True)"},
    });
}
} // namespace destruction
