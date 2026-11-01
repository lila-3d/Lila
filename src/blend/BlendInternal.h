#pragma once

#include "blend/Blend.h"
#include "blend/BlendFile.h"

#include "scene/Graph.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::blend
{
using scene::Graph;
using scene::NodeId;
using scene::kNoNode;

// Blender's enums, as saved.
namespace ob
{
inline constexpr int kEmpty = 0, kMesh = 1, kCurve = 2, kSurface = 3, kFont = 4, kMeta = 5, kLight = 10, kCamera = 11, kSpeaker = 12,
                     kLightProbe = 13, kLattice = 22, kArmature = 25, kGpencilLegacy = 26, kCurves = 27, kPointCloud = 28, kVolume = 29,
                     kGreasePencil = 30;
inline constexpr int kParentObject = 0, kParentSkeleton = 4, kParentVertex = 6, kParentBone = 7, kParentVertex3 = 5;
inline constexpr int kHideViewport = 1 << 0, kHideRender = 1 << 2;
inline constexpr int kDupliCollection = 1 << 8;
} // namespace ob

/** Modifier type numbers (eModifierType). */
namespace md
{
inline constexpr int kSubsurf = 1, kLattice = 2, kCurve = 3, kBuild = 4, kMirror = 5, kDecimate = 6, kWave = 7, kArmature = 8, kHook = 9,
                     kSoftbody = 10, kBoolean = 11, kArray = 12, kEdgeSplit = 13, kDisplace = 14, kUVProject = 15, kSmooth = 16, kCast = 17,
                     kMeshDeform = 18, kParticleSystem = 19, kCloth = 22, kCollision = 23, kBevel = 24, kShrinkwrap = 25, kMask = 27,
                     kSimpleDeform = 28, kMultires = 29, kSurface = 30, kShapeKey = 32, kSolidify = 33, kScrew = 34, kRemesh = 41, kSkin = 42,
                     kLaplacianSmooth = 43, kTriangulate = 44, kWireframe = 48, kDataTransfer = 49, kNormalEdit = 50, kCorrectiveSmooth = 51,
                     kMeshSequenceCache = 52, kSurfaceDeform = 53, kWeightedNormal = 54, kWeld = 55, kNodes = 57;
const char* name(int type);
} // namespace md

/** Blender to Lila: Z up metres to the scene's up axis and centimetres. */
struct Axes
{
    glm::dmat3 F{1.0};
    double cm = 100.0;
    /** Blender axis a lands on Lila axis axis[a], negated when sign[a] < 0. */
    int axis[3] = {0, 1, 2};
    double sign[3] = {1, 1, 1};

    Axes(bool lilaZUp, double cmPerUnit);
    glm::dvec3 point(const glm::dvec3& p) const { return F * p * cm; }
    glm::dvec3 direction(const glm::dvec3& v) const { return F * v; }
    /** A Blender local matrix as the Lila local matrix of the same node. */
    glm::dmat4 matrix(const glm::dmat4& m) const;
    /** Euler radians in a Blender order to Lila degrees in order(o). */
    glm::dvec3 euler(const glm::dvec3& radians) const;
    glm::dvec3 scale(const glm::dvec3& s) const;
    glm::dquat quat(const glm::dquat& q) const;
    /** Blender rotmode (1 XYZ ... 6 ZYX) as the Lila order the converted angles use. */
    scene::RotateOrder order(int rotmode) const;
};

/** One mesh's arrays, from either the attribute storage (5.x) or CustomData (4.x). */
struct MeshAttr
{
    std::string name;
    /** bke::AttrType: 0 bool, 1 int8, 2 int2 (int16 2D), 3 int32, 4 int32 2D, 5 float, 6 float2, 7 float3, 8 float4x4, 9 byte color, 10 float color, 11 quaternion, 12 string. */
    int type = -1;
    /** 0 point, 1 edge, 2 face, 3 corner. */
    int domain = -1;
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    /** A single value for every element. */
    bool single = false;

    /** Bytes of one value of type. */
    static size_t typeSize(int type);
    /** Value i, or the single value; null past the end. */
    const uint8_t* at(size_t i) const
    {
        const size_t s = typeSize(type);
        const size_t off = single ? 0 : i * s;
        return data && off + s <= bytes ? data + off : nullptr;
    }
    bool covers(size_t count) const { return data && (single ? bytes >= typeSize(type) : bytes >= count * typeSize(type)); }
};

/** Everything converted for one Blender object. */
struct ObjectNode
{
    NodeId id = kNoNode;
    /** Where children attach (the object itself unless it got an extra offset parent). */
    NodeId self = kNoNode;
    int type = -1;
    View object;
    /** Bone name to joint, for armatures. */
    std::map<std::string, NodeId> joints;
    /** Joint rest local translation and rotation (Lila), per bone, for posing and animation. */
    struct Rest
    {
        glm::dvec3 t{0.0};
        glm::dmat3 r{1.0};
        double length = 0.0;
        scene::RotateOrder order = scene::RotateOrder::XYZ;
        bool deform = true;
    };
    std::map<std::string, Rest> rest;
    /** Lila order used for the rotate channel. */
    scene::RotateOrder order = scene::RotateOrder::XYZ;
    std::vector<NodeId> morphTargets;
    std::vector<std::string> morphNames;
};

class Importer
{
public:
    Importer(Graph& graph, const File& file, const ImportOptions& opts, ImportResult& r);
    void run();

    Graph& g;
    const File& file;
    const ImportOptions& opts;
    ImportResult& r;
    Axes axes;
    double fps = 24.0;
    View scene;

    /** Object block data pointer to what it became (the first copy; instances add more). */
    std::unordered_map<const uint8_t*, ObjectNode> objects;
    /** Mesh ID data to the built mesh, shared by every object using it. */
    std::unordered_map<const uint8_t*, std::shared_ptr<geo::Mesh>> meshes;
    /** Mesh ID data to the first mesh node built from it, for instancing. */
    std::unordered_map<const uint8_t*, NodeId> meshSources;
    /** Material ID data to the Lila slot name. */
    std::unordered_map<const uint8_t*, std::string> materialNames;
    std::set<std::string> issuesFor;
    /** Parent object data to its children. */
    std::unordered_map<const uint8_t*, std::vector<View>> children;
    /** Objects in a collection the view layer includes. */
    std::set<const uint8_t*> inLayer;
    /** While a collection instance is built: the objects it holds. */
    std::set<const uint8_t*> instanceMembers;

    // BlendImport.cpp
    std::string nameOf(const View& id) const;
    NodeId createNode(const char* type, const std::string& name, NodeId parent);
    void setTransform(NodeId node, const View& object, ObjectNode& on);
    void buildCollection(const View& collection, const View& layerCollection, NodeId parent, std::set<const uint8_t*>& placed, int depth);
    NodeId buildObject(const View& object, NodeId parent, std::set<const uint8_t*>& placed, int depth, bool instance);
    void buildInstance(const View& object, NodeId parent, int depth);
    std::vector<MeshAttr> meshAttributes(const View& mesh) const;
    std::shared_ptr<geo::Mesh> buildMesh(const View& mesh, const View& object);
    void applyCamera(NodeId id, const View& camera);
    void applyLight(NodeId id, const View& light);
    void buildCurve(NodeId parent, const View& object, ObjectNode& on);
    void warnOnce(const std::string& key, const std::string& message);
    void issue(NodeId node, const std::string& message, bool warning = true);

    // BlendMaterials.cpp
    std::vector<std::string> materialSlots(const View& object, const View& data);
    std::string material(const View& material);
    std::string imagePath(const View& image);
    std::map<const uint8_t*, std::string> imagePaths;
    fs::path textureDir;

    // BlendRig.cpp
    void buildArmature(NodeId node, const View& object, ObjectNode& on);
    void buildSkins();
    void buildMorphs();
    void applyPoses();
    void importAnimation();
    struct SkinJob
    {
        NodeId mesh = kNoNode;
        View meshData;
        View object;
        View armature; // the armature object
    };
    std::vector<SkinJob> skinJobs;
    struct MorphJob
    {
        NodeId mesh = kNoNode;
        View meshData;
        ObjectNode* on = nullptr;
    };
    std::vector<MorphJob> morphJobs;

    // BlendModifiers.cpp
    struct ModifierJob
    {
        NodeId mesh = kNoNode;
        View object;
    };
    std::vector<ModifierJob> modifierJobs;
    void buildModifiers();
    /** Hard edges from a "Smooth by Angle" node group or 4.0's auto smooth, applied to the mesh before it is set. */
    void smoothByAngle(const View& object, const View& meshData, geo::Mesh& m);
    void evaluateWithBlender();
    std::vector<std::pair<NodeId, std::string>> toEvaluate;
};

inline std::string utf8(const fs::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

/** The armature object a mesh object deforms with (Armature modifier, or an armature parent with skeleton parenting). */
View armatureOf(const View& object);
} // namespace lila::blend
