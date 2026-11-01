#include "blend/BlendInternal.h"

#include "core/Profile.h"
#include "core/Progress.h"
#include "curve/CurveNodes.h"
#include "fbx/ImportCommon.h"
#include "geo/Mesh.h"
#include "math/MathCore.h"
#include "scene/Camera.h"
#include "scene/Diagnostics.h"
#include "scene/Instancing.h"
#include "scene/Light.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <format>

namespace lila::blend
{
namespace
{
using scene::Transform;

// Collection and layer collection flags.
constexpr int kCollectionHideViewport = 1 << 0;
constexpr int kLayerCollectionExclude = 1 << 4;
constexpr int kLayerCollectionHide = 1 << 7;

constexpr int kMaxDepth = 16;

bool nearIdentity(const glm::dmat4& m, double eps = 1e-6)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(m[c][r] - (c == r ? 1.0 : 0.0)) > eps)
                return false;
    return true;
}

glm::dvec3 clean(glm::dvec3 v)
{
    for (int i = 0; i < 3; ++i)
        if (std::abs(v[i]) < 1e-9)
            v[i] = 0.0;
    return v;
}

const char* orderLetters(int rotmode)
{
    switch (rotmode)
    {
    case 2: return "XZY";
    case 3: return "YXZ";
    case 4: return "YZX";
    case 5: return "ZXY";
    case 6: return "ZYX";
    default: return "XYZ";
    }
}

scene::RotateOrder orderFromLetters(std::string_view s)
{
    static constexpr std::pair<const char*, scene::RotateOrder> kOrders[] = {
        {"XYZ", scene::RotateOrder::XYZ}, {"YZX", scene::RotateOrder::YZX}, {"ZXY", scene::RotateOrder::ZXY},
        {"XZY", scene::RotateOrder::XZY}, {"YXZ", scene::RotateOrder::YXZ}, {"ZYX", scene::RotateOrder::ZYX}};
    for (const auto& [name, order] : kOrders)
        if (s == name)
            return order;
    return scene::RotateOrder::XYZ;
}

/** Blender's rotation of an object or pose channel in its rotation mode, as a matrix. */
glm::dmat3 rotationOf(const View& v, const char* eul, const char* quat, const char* axis, const char* angle, int rotmode)
{
    if (rotmode == 0)
    {
        const glm::dvec4 q = v.vec4(quat, glm::dvec4(1, 0, 0, 0));
        glm::dquat dq(q[0], q[1], q[2], q[3]);
        const double len = glm::length(dq);
        return glm::mat3_cast(len > 0.0 ? dq / len : glm::dquat(1, 0, 0, 0));
    }
    if (rotmode == -1)
    {
        glm::dvec3 ax = v.vec3(axis, glm::dvec3(0, 1, 0));
        const double len = glm::length(ax);
        if (len <= 0.0)
            return glm::dmat3(1.0);
        return glm::dmat3(glm::rotate(glm::dmat4(1.0), v.num(angle), ax / len));
    }
    const glm::dvec3 r = v.vec3(eul);
    return glm::dmat3(scene::eulerToMatrix(glm::degrees(r), orderFromLetters(orderLetters(rotmode))));
}

/** The collection an empty instances (dup_group in the file). */
View instanceCollection(const View& object)
{
    return object.has("instance_collection") ? object.deref("instance_collection") : object.deref("dup_group");
}

bool isAnimated(const View& id)
{
    const View adt = id.deref("adt");
    return adt && adt.ptr("action") != 0;
}
} // namespace

const char* md::name(int type)
{
    switch (type)
    {
    case kSubsurf: return "Subdivision Surface";
    case kLattice: return "Lattice";
    case kCurve: return "Curve";
    case kBuild: return "Build";
    case kMirror: return "Mirror";
    case kDecimate: return "Decimate";
    case kWave: return "Wave";
    case kArmature: return "Armature";
    case kHook: return "Hook";
    case kSoftbody: return "Soft Body";
    case kBoolean: return "Boolean";
    case kArray: return "Array";
    case kEdgeSplit: return "Edge Split";
    case kDisplace: return "Displace";
    case kUVProject: return "UV Project";
    case kSmooth: return "Smooth";
    case kCast: return "Cast";
    case kMeshDeform: return "Mesh Deform";
    case kParticleSystem: return "Particle System";
    case kCloth: return "Cloth";
    case kCollision: return "Collision";
    case kBevel: return "Bevel";
    case kShrinkwrap: return "Shrinkwrap";
    case kMask: return "Mask";
    case kSimpleDeform: return "Simple Deform";
    case kMultires: return "Multiresolution";
    case kSurface: return "Surface";
    case kShapeKey: return "Shape Key";
    case kSolidify: return "Solidify";
    case kScrew: return "Screw";
    case kRemesh: return "Remesh";
    case kSkin: return "Skin";
    case kLaplacianSmooth: return "Laplacian Smooth";
    case kTriangulate: return "Triangulate";
    case kWireframe: return "Wireframe";
    case kDataTransfer: return "Data Transfer";
    case kNormalEdit: return "Normal Edit";
    case kCorrectiveSmooth: return "Corrective Smooth";
    case kMeshSequenceCache: return "Mesh Sequence Cache";
    case kSurfaceDeform: return "Surface Deform";
    case kWeightedNormal: return "Weighted Normal";
    case kWeld: return "Weld";
    case kNodes: return "Geometry Nodes";
    default: return "modifier";
    }
}

// Axes ----------------------------------------------------------------------------------------------------------------

Axes::Axes(bool lilaZUp, double cmPerUnit) : cm(cmPerUnit)
{
    // Y up turns Blender -90 degrees about X: (x, y, z) -> (x, z, -y).
    if (!lilaZUp)
        F = glm::dmat3(glm::dvec3(1, 0, 0), glm::dvec3(0, 0, -1), glm::dvec3(0, 1, 0));
    for (int a = 0; a < 3; ++a)
        for (int k = 0; k < 3; ++k)
            if (std::abs(F[a][k]) > 0.5)
            {
                axis[a] = k;
                sign[a] = F[a][k] > 0.0 ? 1.0 : -1.0;
            }
}

glm::dmat4 Axes::matrix(const glm::dmat4& m) const
{
    glm::dmat4 out(F * glm::dmat3(m) * glm::transpose(F));
    out[3] = glm::dvec4(F * glm::dvec3(m[3]) * cm, 1.0);
    return out;
}

glm::dvec3 Axes::euler(const glm::dvec3& radians) const
{
    glm::dvec3 out(0.0);
    for (int a = 0; a < 3; ++a)
        out[axis[a]] = sign[a] * glm::degrees(radians[a]);
    return out;
}

glm::dvec3 Axes::scale(const glm::dvec3& s) const
{
    glm::dvec3 out(1.0);
    for (int a = 0; a < 3; ++a)
        out[axis[a]] = s[a];
    return out;
}

glm::dquat Axes::quat(const glm::dquat& q) const
{
    const glm::dvec3 v = F * glm::dvec3(q.x, q.y, q.z);
    return glm::dquat(q.w, v.x, v.y, v.z);
}

scene::RotateOrder Axes::order(int rotmode) const
{
    const char* s = orderLetters(rotmode);
    std::string out;
    for (int i = 0; i < 3; ++i)
        out += char('X' + axis[s[i] - 'X']);
    return orderFromLetters(out);
}

// Importer ------------------------------------------------------------------------------------------------------------

Importer::Importer(Graph& graph, const File& f, const ImportOptions& o, ImportResult& result)
    : g(graph), file(f), opts(o), r(result), axes(math::upAxis() == math::UpAxis::Z, 100.0)
{
}

std::string Importer::nameOf(const View& id) const
{
    std::string n = fbx::safeName(id.idName(), "Object");
    return opts.nameSpace.empty() ? n : opts.nameSpace + ":" + n;
}

NodeId Importer::createNode(const char* type, const std::string& name, NodeId parent)
{
    const scene::NodeType* t = scene::types::find(type);
    if (!t)
        t = scene::types::find("transform");
    scene::Node* n = g.createNode(*t, name, parent, false);
    r.nodes.push_back(n->id());
    if (parent == opts.parent)
        r.roots.push_back(n->id());
    return n->id();
}

void Importer::warnOnce(const std::string& key, const std::string& message)
{
    if (issuesFor.insert(key).second)
        r.warnings.push_back(message);
}

void Importer::issue(NodeId node, const std::string& message, bool warning)
{
    const scene::Node* n = g.node(node);
    if (!n)
        return;
    scene::diagnostics::Issue i;
    i.source = "blendImport";
    i.severity = warning ? scene::diagnostics::Severity::Warning : scene::diagnostics::Severity::Info;
    i.target = n->path();
    i.message = message;
    scene::diagnostics::report(std::move(i));
}

void Importer::run()
{
    LILA_ZONE();

    // The scene the file was saved showing, else the first.
    for (const View& wm : file.ids("WM"))
        for (const View& win : wm.list("windows"))
            if (View sc = win.deref("scene"); sc && !scene)
                scene = sc;
    if (!scene)
    {
        const std::vector<View> scenes = file.ids("SC");
        if (scenes.empty())
        {
            r.error = "the file has no scene";
            return;
        }
        scene = scenes.front();
    }
    const double unit = scene.sub("unit").num("scale_length", 0, 1.0);
    axes = Axes(math::upAxis() == math::UpAxis::Z, 100.0 * (unit > 0.0 ? unit : 1.0));
    const View rd = scene.sub("r");
    const double base = rd.num("frs_sec_base", 0, 1.0);
    fps = rd.num("frs_sec", 0, 24.0) / (base > 0.0 ? base : 1.0);
    if (fps <= 0.0)
        fps = 24.0;
    r.version = file.versionString();
    r.fps = fps;
    textureDir = opts.textureDir.empty() ? file.path().parent_path() / (file.path().stem().string() + ".textures")
                                         : fs::path(std::u8string(opts.textureDir.begin(), opts.textureDir.end()));

    const View master = scene.deref("master_collection");
    if (!master)
    {
        r.error = "the scene has no collections";
        return;
    }
    View layerCollection;
    if (const std::vector<View> layers = scene.list("view_layers", "ViewLayer"); !layers.empty())
    {
        if (const std::vector<View> lcs = layers.front().list("layer_collections", "LayerCollection"); !lcs.empty())
            layerCollection = lcs.front();
        // Objects hidden in the view layer (the outliner's eye) keep the flag on their base, not the object.
        for (const View& base : layers.front().list("object_bases", "Base"))
            if (base.i("flag") & (1 << 8))
                hiddenInLayer.insert(base.deref("object").data());
    }

    for (const View& o : file.ids("OB"))
        if (const View p = o.deref("parent"))
            children[p.data()].push_back(o);
    // Objects only in hidden collections stay hidden wherever they end up (under a parent elsewhere, say).
    std::set<const uint8_t*> shownSomewhere;
    std::function<void(const View&, const View&, int, bool)> gather = [&](const View& c, const View& lc, int d, bool hiddenAbove) {
        if (d > kMaxDepth || (lc && (lc.i("flag") & kLayerCollectionExclude)))
            return;
        const bool hidden = hiddenAbove || (c.i("flag") & kCollectionHideViewport) || (lc && (lc.i("flag") & kLayerCollectionHide));
        for (const View& e : c.list("gobject", "CollectionObject"))
        {
            inLayer.insert(e.deref("ob").data());
            if (!hidden)
                shownSomewhere.insert(e.deref("ob").data());
        }
        const std::vector<View> childLayers = lc ? lc.list("layer_collections", "LayerCollection") : std::vector<View>{};
        for (const View& child : c.list("children", "CollectionChild"))
            if (const View sub = child.deref("collection"))
            {
                View sublc;
                for (const View& l : childLayers)
                    if (l.deref("collection").data() == sub.data())
                        sublc = l;
                gather(sub, sublc, d + 1, hidden);
            }
    };
    gather(master, layerCollection, 0, false);
    for (const uint8_t* o : inLayer)
        if (!shownSomewhere.count(o))
            hiddenInLayer.insert(o);

    {
        progress::Scope build("Building the scene", 1.0);
        std::set<const uint8_t*> placed;
        buildCollection(master, layerCollection, opts.parent, placed, 0);
    }
    g.evaluate();
    {
        progress::Scope rig("Rigging", 1.0);
        buildSkins();
        buildMorphs();
    }
    {
        progress::Scope mods("Modifiers", 1.0);
        buildModifiers();
    }
    applyPoses();
    applyObjectConstraints();
    if (opts.animation)
    {
        progress::Scope anim("Animation", 1.0);
        importAnimation();
    }
    evaluateWithBlender();
    if (opts.setPlayback)
    {
        fbx::setPlayback(fps, rd.num("sfra", 0, 1.0) / fps, rd.num("efra", 0, 250.0) / fps);
        // The frame the file was saved on: the stored pose (constraints included) is the rig at that frame.
        g.setTime(rd.num("cfra", 0, rd.num("sfra", 0, 1.0)) / fps);
    }
    r.ok = true;
}

// Collections and objects ---------------------------------------------------------------------------------------------

void Importer::buildCollection(const View& collection, const View& layerCollection, NodeId parent, std::set<const uint8_t*>& placed, int depth)
{
    if (depth > kMaxDepth)
        return;
    const int lcFlag = layerCollection ? layerCollection.i("flag") : 0;
    if (lcFlag & kLayerCollectionExclude)
        return;
    const bool hidden = (collection.i("flag") & kCollectionHideViewport) || (lcFlag & kLayerCollectionHide);
    if (hidden && !opts.hidden)
        return;

    NodeId group = parent;
    if (depth > 0 && opts.collections)
    {
        group = createNode("transform", nameOf(collection), parent);
        if (hidden)
            g.setValue({group, Transform::kVisibility, -1}, false);
    }

    std::vector<View> childLayers = layerCollection ? layerCollection.list("layer_collections", "LayerCollection") : std::vector<View>{};
    for (const View& child : collection.list("children", "CollectionChild"))
    {
        const View sub = child.deref("collection");
        if (!sub)
            continue;
        View lc;
        for (const View& c : childLayers)
            if (c.deref("collection").data() == sub.data())
                lc = c;
        buildCollection(sub, lc, group, placed, depth + 1);
    }

    for (const View& entry : collection.list("gobject", "CollectionObject"))
    {
        const View object = entry.deref("ob");
        // Children come in under their parent, wherever their collection is.
        if (object && !(object.deref("parent") && inLayer.count(object.deref("parent").data())))
            buildObject(object, group, placed, depth, false);
    }
}

NodeId Importer::buildObject(const View& object, NodeId parent, std::set<const uint8_t*>& placed, int depth, bool instance)
{
    if (depth > kMaxDepth || !placed.insert(object.data()).second)
        return kNoNode;
    const int type = object.i("type");
    const int restrict = object.has("visibility_flag") ? object.i("visibility_flag") : object.i("restrictflag");
    // Invisible to the camera: shadow casters and other render tricks, which show as solid walls in a viewport.
    const bool cameraHidden = (restrict & ob::kHideCamera) != 0;
    const bool hidden = (restrict & ob::kHideViewport) || hiddenInLayer.count(object.data()) || cameraHidden;
    if (hidden && !opts.hidden)
        return kNoNode;

    const std::string name = nameOf(object);
    const std::string typeName = [&] {
        switch (type)
        {
        case ob::kGreasePencil:
        case ob::kGpencilLegacy: return std::string("grease pencil");
        case ob::kCurves: return std::string("hair curves");
        case ob::kPointCloud: return std::string("point cloud");
        case ob::kVolume: return std::string("volume");
        case ob::kLattice: return std::string("lattice");
        case ob::kFont: return std::string("text");
        case ob::kSurface: return std::string("NURBS surface");
        case ob::kMeta: return std::string("metaball");
        case ob::kSpeaker: return std::string("speaker");
        case ob::kLightProbe: return std::string("light probe");
        default: return std::string();
        }
    }();
    if (!typeName.empty())
        r.skipped.push_back(std::format("{} object '{}' (kept as an empty group)", typeName, object.idName()));

    // An offset parent keeps animated channels exact when Blender's parent inverse isn't identity.
    const View parentObject = object.deref("parent");
    const bool animated = isAnimated(object);
    glm::dmat4 pre(1.0);
    if (parentObject)
    {
        pre = object.mat4("parentinv");
        if ((object.i("partype") & 15) == ob::kParentBone)
        {
            const std::string bone = object.str("parsubstr");
            if (auto it = objects.find(parentObject.data()); it != objects.end())
            {
                if (auto rb = it->second.rest.find(bone); rb != it->second.rest.end())
                    pre = glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, rb->second.length, 0.0)) * pre;
                if (auto jt = it->second.joints.find(bone); jt != it->second.joints.end())
                    parent = jt->second;
            }
        }
        else if ((object.i("partype") & 15) == ob::kParentVertex || (object.i("partype") & 15) == ob::kParentVertex3)
            warnOnce("vertexParent:" + object.idName(), std::format("'{}' is parented to vertices; parented to the object instead", object.idName()));
    }
    // Under a camera or light the turn their own axes got is undone.
    bool underTurned = false;
    if (const scene::Node* pn = g.node(parent))
        underTurned = pn->type().isA("camera") || pn->type().isA("light");
    if (underTurned)
        pre = glm::dmat4(glm::transpose(axes.F)) * pre;
    if (!nearIdentity(pre) && animated)
    {
        const NodeId offset = createNode("transform", name + "Offset", parent);
        const fbx::Trs trs = fbx::decompose(underTurned ? glm::dmat4(glm::transpose(axes.F)) * axes.matrix(object.mat4("parentinv"))
                                                        : axes.matrix(pre));
        g.setValue({offset, Transform::kTranslate, -1}, clean(trs.t));
        g.setValue({offset, Transform::kRotate, -1}, clean(trs.r));
        g.setValue({offset, Transform::kScale, -1}, trs.s);
        parent = offset;
        pre = glm::dmat4(1.0);
    }

    ObjectNode on;
    on.object = object;
    on.type = type;
    const View data = object.deref("data");
    const char* nodeType = "transform";
    switch (type)
    {
    case ob::kMesh: nodeType = "mesh"; break;
    case ob::kCamera: nodeType = "camera"; break;
    case ob::kLight: nodeType = "light"; break;
    case ob::kCurve: nodeType = "curve"; break;
    case ob::kEmpty: nodeType = (object.i("transflag") & ob::kDupliCollection) && instanceCollection(object) ? "transform" : "locator"; break;
    default: break;
    }
    const NodeId id = createNode(nodeType, name, parent);
    on.id = on.self = id;
    if (hidden)
        g.setValue({id, Transform::kVisibility, -1}, false);
    if (cameraHidden)
        issue(id, "Hidden: Blender's camera doesn't see it (a shadow caster or other render helper).", false);

    // Transform.
    {
        const int rotmode = object.i("rotmode");
        const glm::dvec3 loc = object.vec3("loc") + object.vec3("dloc");
        glm::dvec3 size = object.vec3("size", glm::dvec3(1.0));
        if (object.has("dscale"))
            size *= object.vec3("dscale", glm::dvec3(1.0));
        const glm::dvec3 drot = object.vec3("drot");
        const glm::dvec4 dquat = object.vec4("dquat", glm::dvec4(1, 0, 0, 0));
        const bool deltaRotation = glm::length(drot) > 1e-9 || std::abs(dquat[0] - 1.0) > 1e-9;
        const bool euler = rotmode >= 1 && rotmode <= 6;
        if (nearIdentity(pre) && !(deltaRotation && euler))
        {
            on.order = axes.order(euler ? rotmode : 1);
            glm::dvec3 rot;
            if (euler)
                rot = axes.euler(object.vec3("rot"));
            else
            {
                glm::dmat3 R = rotationOf(object, "rot", "quat", "rotAxis", "rotAngle", rotmode);
                if (deltaRotation)
                    R = glm::mat3_cast(glm::normalize(glm::dquat(dquat[0], dquat[1], dquat[2], dquat[3]))) * R;
                rot = scene::matrixToEuler(axes.F * R * glm::transpose(axes.F), on.order);
            }
            g.setValue({id, Transform::kRotateOrder, -1}, int64_t(on.order));
            g.setValue({id, Transform::kTranslate, -1}, clean(axes.point(loc)));
            g.setValue({id, Transform::kRotate, -1}, clean(rot));
            g.setValue({id, Transform::kScale, -1}, axes.scale(size));
        }
        else
        {
            glm::dmat3 R = rotationOf(object, "rot", "quat", "rotAxis", "rotAngle", rotmode);
            if (euler && deltaRotation)
                R = glm::dmat3(scene::eulerToMatrix(glm::degrees(drot), orderFromLetters(orderLetters(rotmode)))) * R;
            glm::dmat4 basis = glm::translate(glm::dmat4(1.0), loc) * glm::dmat4(R) * glm::scale(glm::dmat4(1.0), size);
            const glm::dmat4 local = underTurned ? glm::dmat4(glm::transpose(axes.F)) * axes.matrix(object.mat4("parentinv") * basis)
                                                 : axes.matrix(pre * basis);
            const fbx::Trs trs = fbx::decompose(local);
            if (trs.sheared)
                warnOnce("shear:" + object.idName(), std::format("'{}': shear from its parenting dropped", object.idName()));
            on.order = scene::RotateOrder::XYZ;
            g.setValue({id, Transform::kTranslate, -1}, clean(trs.t));
            g.setValue({id, Transform::kRotate, -1}, clean(trs.r));
            g.setValue({id, Transform::kScale, -1}, trs.s);
        }
        // Cameras and lights look down their local -Z in both, so their own axes turn back.
        if ((type == ob::kCamera || type == ob::kLight) && glm::dmat3(1.0) != axes.F)
            g.setValue({id, Transform::kRotateAxis, -1}, glm::dvec3(-90.0, 0.0, 0.0));
    }

    switch (type)
    {
    case ob::kMesh:
        if (data)
        {
            r.meshes.push_back(id);
            if (auto src = meshSources.find(data.data()); src != meshSources.end())
                scene::instancing::connectInstance(g, src->second, id);
            else
            {
                std::shared_ptr<geo::Mesh> m = buildMesh(data, object);
                if (m && !m->materialNames().empty() &&
                    std::all_of(m->materialNames().begin(), m->materialNames().end(), [&](const std::string& s) { return invisibleMaterials.count(s) != 0; }))
                    invisibleMeshes.insert(data.data());
                if (m)
                {
                    g.setValue({id, scene::MeshNode::kInMesh, -1}, scene::MeshPtr(m));
                    meshSources[data.data()] = id;
                    if (opts.skins && armatureOf(object))
                        skinJobs.push_back({id, data, object, armatureOf(object)});
                    if (opts.morphs && data.ptr("key"))
                        morphJobs.push_back({id, data, nullptr});
                    if (!object.list("modifiers").empty())
                        modifierJobs.push_back({id, object});
                }
            }
        }
        break;
    case ob::kCamera:
        if (data)
            applyCamera(id, data);
        break;
    case ob::kLight:
        if (data)
            applyLight(id, data);
        break;
    case ob::kCurve:
        if (data)
        {
            buildCurve(id, object, on);
            // Geometry nodes on a curve usually make the mesh shown (ground, paths): only Blender can make it.
            if (!object.list("modifiers").empty() && opts.evaluate != "off" && !instance)
                toEvaluate.push_back({id, object.idName(), true, materialSlots(object, data)});
        }
        break;
    case ob::kArmature:
        if (data)
            buildArmature(id, object, on);
        break;
    default: break;
    }
    if (type == ob::kMesh && data && invisibleMeshes.count(data.data()))
    {
        g.setValue({id, Transform::kVisibility, -1}, false);
        issue(id, "Hidden: its materials draw nothing in Blender (transparent or volume only).", false);
    }
    if (!instance || !objects.count(object.data()))
    {
        r.objectNames[object.idName()] = g.node(id)->path();
        auto [it, added] = objects.emplace(object.data(), std::move(on));
        for (MorphJob& j : morphJobs)
            if (j.mesh == id)
                j.on = &it->second;
    }

    if (type == ob::kEmpty && (object.i("transflag") & ob::kDupliCollection))
        buildInstance(object, id, depth + 1);

    // Children: every object parented to this one that is in the scene (or in the instanced collection).
    if (auto kids = children.find(object.data()); kids != children.end())
        for (const View& child : kids->second)
            if (instance ? instanceMembers.count(child.data()) != 0 : inLayer.count(child.data()) != 0)
                buildObject(child, id, placed, depth + 1, instance);
    return id;
}

void Importer::buildInstance(const View& object, NodeId parent, int depth)
{
    const View collection = instanceCollection(object);
    if (!collection || depth > kMaxDepth)
        return;
    const glm::dvec3 offset = collection.has("instance_offset") ? collection.vec3("instance_offset") : collection.vec3("dupli_ofs");
    NodeId group = createNode("transform", nameOf(collection), parent);
    g.setValue({group, Transform::kTranslate, -1}, clean(-axes.point(offset)));

    // Every object of the collection and its children collections, under the group.
    std::set<const uint8_t*> placed;
    const std::set<const uint8_t*> outer = std::exchange(instanceMembers, {});
    std::function<void(const View&, int)> gather = [&](const View& c, int d) {
        if (d > kMaxDepth)
            return;
        for (const View& e : c.list("gobject", "CollectionObject"))
            instanceMembers.insert(e.deref("ob").data());
        for (const View& child : c.list("children", "CollectionChild"))
            if (const View sub = child.deref("collection"))
                gather(sub, d + 1);
    };
    gather(collection, 0);
    std::function<void(const View&, NodeId, int)> add = [&](const View& c, NodeId under, int d) {
        if (d > kMaxDepth)
            return;
        for (const View& child : c.list("children", "CollectionChild"))
            if (const View sub = child.deref("collection"))
                add(sub, under, d + 1);
        std::vector<View> obs;
        for (const View& e : c.list("gobject", "CollectionObject"))
            if (const View o = e.deref("ob"))
                obs.push_back(o);
        for (const View& o : obs)
        {
            const View p = o.deref("parent");
            bool parentHere = false;
            for (const View& q : obs)
                parentHere = parentHere || (p && q.data() == p.data());
            if (!parentHere)
                buildObject(o, under, placed, depth + d, true);
        }
    };
    add(collection, group, 0);
    instanceMembers = outer;
}

// Cameras and lights ---------------------------------------------------------------------------------------------------

void Importer::applyCamera(NodeId id, const View& ca)
{
    using scene::Camera;
    auto set = [&](int attr, scene::PlugValue v) { g.setValue({id, attr, -1}, std::move(v)); };
    const View rd = scene.sub("r");
    const double w = rd.num("xsch", 0, 1920.0) * rd.num("xasp", 0, 1.0), h = rd.num("ysch", 0, 1080.0) * rd.num("yasp", 0, 1.0);
    const double aspect = h > 0.0 ? w / h : 16.0 / 9.0;
    const double sx = ca.num("sensor_x", 0, 36.0), sy = ca.num("sensor_y", 0, 24.0);
    const int fit = ca.i("sensor_fit");
    double hfa, vfa;
    bool vertical;
    if (fit == 2)
    {
        vfa = sy;
        hfa = sy * aspect;
        vertical = true;
    }
    else if (fit == 1 || aspect >= 1.0)
    {
        hfa = sx;
        vfa = sx / aspect;
        vertical = false;
    }
    else
    {
        vfa = sx;
        hfa = sx * aspect;
        vertical = true;
    }
    set(Camera::kFocalLength, ca.num("lens", 0, 50.0));
    set(Camera::kHorizontalFilmAperture, hfa / 25.4);
    set(Camera::kVerticalFilmAperture, vfa / 25.4);
    set(Camera::kFilmFit, int64_t(vertical ? scene::FilmFit::Vertical : scene::FilmFit::Horizontal));
    set(Camera::kNearClipPlane, ca.num("clipsta", 0, 0.1) * axes.cm);
    set(Camera::kFarClipPlane, ca.num("clipend", 0, 1000.0) * axes.cm);
    const int type = ca.i("type");
    if (type == 1)
    {
        set(Camera::kOrthographic, true);
        set(Camera::kOrthographicWidth, ca.num("ortho_scale", 0, 6.0) * axes.cm * (vertical ? aspect : 1.0));
    }
    else if (type == 2)
        warnOnce("panorama", "panoramic cameras come in as perspective ones");
    if (std::abs(ca.num("shiftx")) > 1e-6 || std::abs(ca.num("shifty")) > 1e-6)
        warnOnce("shift:" + ca.idName(), std::format("camera '{}': lens shift not converted", ca.idName()));
    if (const View dof = ca.sub("dof"))
        if (dof.i("flag") & 1)
        {
            set(Camera::kDepthOfField, true);
            set(Camera::kFStop, dof.num("aperture_fstop", 0, 2.8));
            set(Camera::kFocusDistance, dof.num("focus_distance", 0, 10.0) * axes.cm);
        }
    r.cameras.push_back(id);
}

void Importer::applyLight(NodeId id, const View& la)
{
    namespace lights = scene::lights;
    lights::Settings s;
    s.color = glm::dvec3(la.num("r", 0, 1.0), la.num("g", 0, 1.0), la.num("b", 0, 1.0));
    double energy = la.num("energy", 0, 10.0);
    if (la.has("exposure"))
        energy *= std::exp2(la.num("exposure"));
    s.intensity = energy;
    s.units = lights::Units::Watts;
    s.castShadows = (la.i("mode") & 1) != 0;
    const double radius = (la.has("radius") ? la.num("radius") : la.num("area_size")) * axes.cm;
    switch (la.i("type"))
    {
    case 1:
        s.kind = lights::Kind::Directional;
        s.units = lights::Units::Lux;
        s.sourceRadius = glm::degrees(la.num("sun_angle")) * 0.5;
        s.sourceFormat = "blend:sun";
        break;
    case 2:
        s.kind = lights::Kind::Spot;
        s.outerConeAngle = glm::degrees(la.num("spotsize", 0, 0.785)) * 0.5;
        s.innerConeAngle = s.outerConeAngle * (1.0 - std::clamp(la.num("spotblend"), 0.0, 1.0));
        s.sourceRadius = radius;
        s.sourceFormat = "blend:spot";
        break;
    case 4:
    {
        const int shape = la.i("area_shape");
        s.kind = shape == 2 || shape == 3 ? lights::Kind::Disc : lights::Kind::Area;
        s.width = la.num("area_size", 0, 1.0) * axes.cm;
        s.height = (shape == 1 || shape == 3 ? la.num("area_sizey", 0, 1.0) : la.num("area_size", 0, 1.0)) * axes.cm;
        s.sourceFormat = "blend:area";
        break;
    }
    default:
        s.kind = lights::Kind::Point;
        s.sourceRadius = radius;
        s.sourceFormat = "blend:point";
        break;
    }
    lights::setSettings(g, id, s);
    r.lights.push_back(id);
}

// Curves --------------------------------------------------------------------------------------------------------------

void Importer::buildCurve(NodeId id, const View& object, ObjectNode&)
{
    const View cu = object.deref("data");
    curve::Splines splines;
    auto handle = [](int h) {
        switch (h)
        {
        case 0: return curve::HandleType::Free;
        case 2: return curve::HandleType::Vector;
        case 3: return curve::HandleType::Aligned;
        default: return curve::HandleType::Auto;
        }
    };
    for (const View& nu : cu.list("nurb", "Nurb"))
    {
        const int type = nu.i("type") & 7;
        const int nu_ = nu.i("pntsu"), nv = std::max(1, nu.i("pntsv"));
        if (nv > 1)
        {
            warnOnce("surface:" + object.idName(), std::format("'{}': NURBS surface patches not converted", object.idName()));
            continue;
        }
        curve::Spline s;
        s.closed = (nu.i("flagu") & 1) != 0;
        if (type == 1)
        {
            s.kind = curve::Kind::Bezier;
            for (const View& bt : nu.array("bezt", size_t(nu_), "BezTriple"))
            {
                auto at = [&](int k) { return axes.point(glm::dvec3(bt.num("vec", size_t(k * 3)), bt.num("vec", size_t(k * 3 + 1)), bt.num("vec", size_t(k * 3 + 2)))); };
                s.handleL.push_back(at(0));
                s.points.push_back(at(1));
                s.handleR.push_back(at(2));
                s.typeL.push_back(handle(bt.i("h1")));
                s.typeR.push_back(handle(bt.i("h2")));
                s.tilt.push_back(bt.num("tilt", 0, bt.num("alfa")));
                s.radius.push_back(bt.num("radius", 0, 1.0));
            }
        }
        else
        {
            s.kind = type == 4 ? curve::Kind::Nurbs : curve::Kind::Poly;
            for (const View& bp : nu.array("bp", size_t(nu_), "BPoint"))
            {
                s.points.push_back(axes.point(glm::dvec3(bp.num("vec", 0), bp.num("vec", 1), bp.num("vec", 2))));
                s.weights.push_back(bp.num("vec", 3, 1.0));
                s.tilt.push_back(bp.num("tilt", 0, bp.num("alfa")));
                s.radius.push_back(bp.num("radius", 0, 1.0));
            }
            if (s.kind == curve::Kind::Nurbs)
            {
                s.degree = std::clamp(nu.i("orderu") - 1, 1, 7);
                s.degree = std::min<int>(s.degree, int(s.points.size()) - 1);
                const bool endpoint = (nu.i("flagu") & 2) != 0;
                if (!s.closed && !endpoint)
                    for (size_t k = 0; k < s.points.size() + size_t(s.degree) + 1; ++k)
                        s.knots.push_back(double(k));
            }
            else
                s.weights.clear();
        }
        if (s.points.size() >= 2)
            splines.push_back(std::move(s));
    }
    g.setValue({id, curve::CurveShape::kCreate, -1}, curve::makeCurveData(std::move(splines)));
    r.curves.push_back(id);
    if (cu.num("ext1") > 0.0 || cu.num("ext2") > 0.0 || cu.ptr("bevobj"))
        warnOnce("curveGeometry:" + object.idName(), std::format("curve '{}': its extrude / bevel geometry is not converted (the curve is)", object.idName()));
}
} // namespace lila::blend
