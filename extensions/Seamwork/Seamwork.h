#pragma once

#include "Seams.h"

#include <lila/Graph.h>
#include <lila/Node.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

/** The extension's scene side: the seam node, its live outputs and the commands and UI built on them. */
namespace seamwork
{
using lila::scene::Graph;
using lila::scene::Node;
using lila::scene::NodeId;
using json = nlohmann::json;

inline constexpr const char* kSeamType = "seam";
inline constexpr const char* kFieldModifier = "seamFieldModifier";
inline constexpr const char* kContactMaskNode = "SeamContactMask";
inline constexpr const char* kStripType = "seamStrip";
inline constexpr const char* kSkirtType = "seamSkirt";

/** Per-object flags on the seam node's objectFlags elements. */
enum ObjectFlag : int64_t
{
    kFlagGround = 1,
    kFlagIgnore = 2,
};

/** What a seam node computed: the joins, the inputs it saw and per-object distance fields. Kind "seamwork". */
struct SeamData final : lila::scene::Data
{
    std::shared_ptr<const Result> result;
    std::vector<Object> objects;
    std::shared_ptr<const Ground> ground;
    /** How the terrain was read, for stats: heightfield, mesh or none. */
    std::string groundMode = "none";
    Band band;
    double fieldRange = 200.0;
    std::vector<SeamPoint> points;
    /** Per object, per point: distance to its nearest join, capped at fieldRange. */
    std::vector<std::vector<float>> distances;
    /** Content hash of the curves, so listeners can tell a real change from a recompute. */
    uint64_t revision = 0;
    json stats = json::object();
    glm::dvec3 up{0.0, 1.0, 0.0};

    const char* kind() const override { return "seamwork"; }
    nlohmann::json toJson() const override { return stats; }
    size_t bytes() const override;
};

/** The seam data a node's outSeams carries, or null. */
const SeamData* seamDataOf(const Node& seamNode);
const SeamData* seamDataOf(const lila::scene::PlugValue& v);
/** Evaluates and returns the data of a seam node found by name; throws CommandError naming cmd. */
std::shared_ptr<const SeamData> seamDataNamed(Graph& g, const std::string& name, const std::string& cmd, NodeId* idOut = nullptr);
/** The seam node a node names: itself, or the seam node feeding its curves, points or a seamStrip/seamSkirt. */
NodeId seamNodeOf(Graph& g, NodeId id);

/** Rebuilds the band from a node's width, falloff, falloffCurve, noise, noiseScale and seed attrs (the names the seam node uses). */
Band bandOf(const Node& n);

/** A heightfield rebuilt from a terrain's output mesh when it is one regular grid; null (with why) otherwise. */
std::shared_ptr<const Heightfield> heightfieldFromMesh(const Mesh& m, std::string* why);
/** A mesh's tree, shared by every instance of it and kept while the mesh lives. */
std::shared_ptr<const MeshBvh> bvhOf(const std::shared_ptr<const Mesh>& mesh);

bool registerNodes(lila::ext::Context& ctx);
/** Field modifier and terrain Contact Mask node, which carry the live fields and masks. */
bool registerFieldNodes(lila::ext::Context& ctx);
/** Writes each seam node's fields into its meshes' field modifiers and its terrain mask when its joins changed. */
void applyLive(Graph& g, bool force = false);
/** applyLive for one seam node; returns {"fields": n, "terrainMask": bool}. */
json applySeam(Graph& g, NodeId seam, bool force);
/** Runs applyLive whenever the app idles. */
void registerLiveUpdates(lila::ext::Context& ctx);

void registerCommands(lila::ext::Context& ctx);
/** S1: Seam Points and Distribute Along Seam GN nodes, presets, proximity rules, seamScatter. */
bool registerScatterNodes(lila::ext::Context& ctx);
void registerScatterCommands(lila::ext::Context& ctx);
/** S2: seamStrip node and seamDecal. */
bool registerStripNodes(lila::ext::Context& ctx);
void registerStripCommands(lila::ext::Context& ctx);
/** S4: seamSkirt node and command. */
bool registerSkirtNodes(lila::ext::Context& ctx);
void registerSkirtCommands(lila::ext::Context& ctx);
/** S5 and the Unreal side of S2-S4: seamExport, seamSend, materials. */
void registerExportCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);

/** The nearest surface of the data's objects or terrain along dir within range. */
bool raySurface(const SeamData& d, const glm::dvec3& from, const glm::dvec3& dir, glm::dvec3& hit, glm::dvec3& normal, double range);

/** json [x, y, z]. */
inline json v3(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }
/** An object's display name in the data, "terrain" for kTerrain, "" for kNone. */
std::string objectName(const SeamData& d, int index);
} // namespace seamwork
