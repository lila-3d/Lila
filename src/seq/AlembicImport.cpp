#include "seq/Alembic.h"
#include "seq/MeshSequence.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "fbx/ImportCommon.h"
#include "core/Paths.h"
#include "gn/Geometry.h"
#include "io/FileTranslators.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshCache.h"
#include "scene/MeshNodes.h"

#include <algorithm>
#include <format>

namespace lila::seq
{
namespace
{
using T = ArgType;

struct Imported
{
    std::vector<scene::NodeId> nodes;
    std::vector<scene::NodeId> caches;
    std::vector<std::string> warnings;
    double start = 1e300, end = -1e300;
};

/** A mesh per PolyMesh at its first sample, and a meshCache playing the rest when its topology holds. */
Imported build(scene::Graph& g, std::vector<abc::MeshCache>& caches, const glm::dmat4& toScene, scene::NodeId parent)
{
    LILA_ZONE();

    Imported r;
    const scene::NodeType* meshType = scene::types::find("mesh");
    for (abc::MeshCache& c : caches)
    {
        if (c.rest.numFaces() == 0)
        {
            r.warnings.push_back(std::format("{}: no faces; skipped", c.path));
            continue;
        }
        gn::transformMesh(c.rest, toScene);
        scene::Node* n = g.createNode(*meshType, c.name.empty() ? std::string("mesh") : c.name, parent, true);
        g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(c.rest))));
        r.nodes.push_back(n->id());
        if (c.topologyChanges)
            r.warnings.push_back(std::format("{}: the topology changes over time; imported at its first sample", c.path));
        if (c.points.size() < 2)
            continue;
        auto samples = std::make_shared<scene::MeshCacheSamples>();
        for (size_t k = 0; k < c.points.size(); ++k)
        {
            for (glm::dvec3& p : c.points[k])
                p = glm::dvec3(toScene * glm::dvec4(p, 1.0));
            samples->add(c.times[k], c.points[k]);
        }
        r.start = std::min(r.start, samples->times.front());
        r.end = std::max(r.end, samples->times.back());
        r.caches.push_back(scene::meshCache::create(g, n->id(), std::move(samples)));
    }
    return r;
}

json names(scene::Graph& g, const std::vector<scene::NodeId>& ids)
{
    json out = json::array();
    for (scene::NodeId id : ids)
        out.push_back(g.node(id)->name());
    return out;
}
} // namespace

void registerAlembicImport()
{
    ops::registerCommand({
        .name = "importAlembic",
        .doc = "Import an Alembic (.abc) file's PolyMeshes as meshes, each placed through its transforms at its first sample. A "
               "mesh whose points move over time gets a meshCache deformer playing every sample (interpolated between them); one "
               "whose topology changes keeps its first sample. Returns the new mesh names, or with report=True a dict of nodes, "
               "meshCaches, start, end and warnings.",
        .positionalDoc = "path",
        .flags = {{"profile", "ax", T::String, "ue5",
                   "Axis profile the file was written in, as exportAlembic's: ue5, ue6 or generic (the scene's axes).", {"ue5", "ue6", "generic"}},
                  {"scale", "sc", T::Float, 1.0, "Multiplies every length (10 undoes an export at 0.1)."},
                  {"parent", "p", T::String, nullptr, "Parent the new meshes under this node."},
                  {"setPlayback", "", T::Bool, true, "Set the playback range to the caches' samples."},
                  {"report", "", T::Bool, false, "Return the import report instead of just the names."},
                  {"select", "", T::Bool, true, "Select the new meshes."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("importAlembic: path required");
            const std::string path = a.positional[0].get<std::string>();
            const std::string profile = a.get<std::string>("profile", "ue5");
            if (profile != "ue5" && profile != "ue6" && profile != "generic")
                throw CommandError("importAlembic: profile must be ue5, ue6 or generic");
            const double scale = a.get<double>("scale", 1.0);
            if (!(scale > 0.0))
                throw CommandError("importAlembic: scale must be positive");
            scene::Graph& g = scene::Graph::current();
            scene::NodeId parent = scene::kNoNode;
            if (a.has("parent"))
            {
                scene::Node* p = g.find(a.get<std::string>("parent"));
                if (!p)
                    throw CommandError("importAlembic: no node named " + a.get<std::string>("parent"));
                parent = p->id();
            }

            std::vector<abc::MeshCache> caches;
            std::string err;
            {
                progress::Scope scope("Importing " + path + "...");
                progress::runOnWorker([&] { caches = abc::readCaches(paths::fromUtf8(path), &err); });
            }
            if (!err.empty())
                throw CommandError("importAlembic: " + path + ": " + err);
            if (caches.empty())
                throw CommandError("importAlembic: " + path + " has no PolyMeshes");

            const glm::dmat4 toScene = glm::dmat4(glm::dmat3(scale) * glm::transpose(fileBasis(profile)));
            Imported r = build(g, caches, toScene, parent);
            for (const std::string& w : r.warnings)
                log::warn("importAlembic: {}", w);
            if (a.get<bool>("setPlayback", true) && r.end > r.start)
                fbx::setPlayback(scene::anim::playback().fps, r.start, r.end);
            if (a.get<bool>("select", true))
                g.select(r.nodes);
            if (!a.get<bool>("report", false))
                return names(g, r.nodes);
            return {{"nodes", names(g, r.nodes)},
                    {"meshCaches", names(g, r.caches)},
                    {"start", r.end >= r.start ? json(r.start) : json(nullptr)},
                    {"end", r.end >= r.start ? json(r.end) : json(nullptr)},
                    {"warnings", r.warnings}};
        },
    });

    if (!io::translators::find("abc"))
        io::translators::add({.id = "abc",
                              .label = "Alembic",
                              .doc = "Alembic caches: PolyMeshes come in as meshes, animated points as a meshCache deformer.",
                              .extensions = {".abc"},
                              .import = {.command = "importAlembic"}});
}
} // namespace lila::seq
