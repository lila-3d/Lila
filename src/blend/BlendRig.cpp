#include "blend/BlendInternal.h"

#include "core/Profile.h"
#include "fbx/ImportCommon.h"
#include "scene/AnimCurve.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/MeshNodes.h"
#include "scene/Morphs.h"
#include "scene/RotationCurve.h"
#include "scene/Rig.h"
#include "scene/Skin.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <numbers>

namespace lila::blend
{
namespace
{
using scene::Transform;
namespace anim = scene::anim;

constexpr int kBoneNoDeform = 1 << 22;
constexpr int kKeyBlockMute = 1 << 0;

/** The parts of an F-Curve's data path: 'pose.bones["Arm"].rotation_euler' gives ("pose.bones", "Arm", "rotation_euler"). */
struct Path
{
    std::string collection;
    std::string key;
    std::string property;
};

Path parsePath(const std::string& s)
{
    Path p;
    const size_t open = s.find("[\"");
    if (open == std::string::npos)
    {
        p.property = s;
        return p;
    }
    p.collection = s.substr(0, open);
    size_t i = open + 2;
    for (; i < s.size() && s[i] != '"'; ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
            ++i;
        p.key += s[i];
    }
    const size_t dot = s.find('.', i);
    p.property = dot == std::string::npos ? std::string() : s.substr(dot + 1);
    return p;
}
} // namespace

View armatureOf(const View& object)
{
    for (const View& mod : object.list("modifiers"))
    {
        const View head = mod.has("modifier") ? mod.sub("modifier") : mod;
        if (head.i("type") == md::kArmature && (head.i("mode") & 1))
            if (const View arm = mod.deref("object"); arm && arm.i("type") == ob::kArmature)
                return arm;
    }
    if (const View p = object.deref("parent"); p && p.i("type") == ob::kArmature && (object.i("partype") & 15) == ob::kParentSkeleton)
        return p;
    return {};
}

void Importer::buildArmature(NodeId node, const View& object, ObjectNode& on)
{
    const View arm = object.deref("data");
    std::function<void(const View&, NodeId, const glm::dmat4&, int)> add = [&](const View& bone, NodeId parent, const glm::dmat4& parentArm, int depth) {
        if (depth > 256)
            return;
        const std::string boneName = bone.str("name");
        const glm::dmat4 armMat = bone.mat4("arm_mat");
        const glm::dmat4 L = axes.matrix(glm::inverse(parentArm) * armMat);
        const NodeId j = createNode("joint", fbx::safeName(boneName, "joint"), parent);
        ObjectNode::Rest rest;
        rest.t = glm::dvec3(L[3]);
        rest.r = glm::dmat3(L);
        rest.length = bone.num("length");
        rest.deform = !(bone.i("flag") & kBoneNoDeform);
        g.setValue({j, Transform::kTranslate, -1}, rest.t);
        g.setValue({j, scene::Joint::kJointOrient, -1}, scene::matrixToEuler(rest.r, scene::RotateOrder::XYZ));
        g.setValue({j, scene::Joint::kRadius, -1}, std::max(0.5, rest.length * axes.cm * 0.08));
        on.joints[boneName] = j;
        on.rest[boneName] = rest;
        r.joints.push_back(j);
        for (const View& child : bone.list("childbase", "Bone"))
            add(child, j, armMat, depth + 1);
    };
    for (const View& bone : arm.list("bonebase", "Bone"))
        add(bone, node, glm::dmat4(1.0), 0);
}

void Importer::applyPoses()
{
    for (auto& [data, on] : objects)
    {
        if (on.type != ob::kArmature)
            continue;
        const View pose = on.object.deref("pose");
        if (!pose)
            continue;
        for (const View& ch : pose.list("chanbase", "bPoseChannel"))
        {
            const std::string name = ch.str("name");
            auto jt = on.joints.find(name);
            auto rt = on.rest.find(name);
            if (jt == on.joints.end() || rt == on.rest.end())
                continue;
            ObjectNode::Rest& rest = rt->second;
            const NodeId j = jt->second;
            const int rotmode = ch.i("rotmode");
            glm::dvec3 rot;
            if (rotmode >= 1 && rotmode <= 6)
            {
                rest.order = axes.order(rotmode);
                rot = axes.euler(ch.vec3("eul"));
            }
            else
            {
                rest.order = axes.order(1);
                glm::dmat3 R(1.0);
                if (rotmode == 0)
                {
                    const glm::dvec4 q = ch.vec4("quat", glm::dvec4(1, 0, 0, 0));
                    const glm::dquat dq(q[0], q[1], q[2], q[3]);
                    R = glm::mat3_cast(glm::length(dq) > 0.0 ? glm::normalize(dq) : glm::dquat(1, 0, 0, 0));
                }
                else
                {
                    const glm::dvec3 ax = ch.vec3("rotAxis", glm::dvec3(0, 1, 0));
                    if (glm::length(ax) > 0.0)
                        R = glm::dmat3(glm::rotate(glm::dmat4(1.0), ch.num("rotAngle"), glm::normalize(ax)));
                }
                rot = scene::matrixToEuler(axes.F * R * glm::transpose(axes.F), rest.order);
            }
            g.setValue({j, Transform::kRotateOrder, -1}, int64_t(rest.order));
            // The pose Blender evaluated when it saved, constraints and drivers included: deform bones driven by
            // constraints keep zero channels, so only this puts them where the rig shows them.
            if (const glm::dmat4 P = ch.mat4("pose_mat"); ch.has("pose_mat") && std::abs(glm::determinant(glm::dmat3(P))) > 1e-9)
            {
                glm::dmat4 local = P;
                if (const View parent = ch.deref("parent"))
                {
                    const glm::dmat4 Pp = parent.mat4("pose_mat");
                    if (std::abs(glm::determinant(glm::dmat3(Pp))) > 1e-9)
                        local = glm::inverse(Pp) * P;
                }
                const fbx::Trs trs = fbx::decomposeWith(axes.matrix(local), rest.r, rest.order, rot);
                g.setValue({j, Transform::kRotate, -1}, trs.r);
                g.setValue({j, Transform::kTranslate, -1}, trs.t);
                g.setValue({j, Transform::kScale, -1}, trs.s);
                continue;
            }
            g.setValue({j, Transform::kRotate, -1}, rot);
            g.setValue({j, Transform::kTranslate, -1}, rest.t + rest.r * axes.point(ch.vec3("loc")));
            g.setValue({j, Transform::kScale, -1}, axes.scale(ch.vec3("size", glm::dvec3(1.0))));
        }
    }
}

void Importer::applyObjectConstraints()
{
    constexpr int kCopyTransforms = 23;
    bool evaluated = false;
    for (auto& [data, on] : objects)
        for (const View& con : on.object.list("constraints", "bConstraint"))
        {
            if (con.num("enforce", 0, 1.0) <= 0.0 || (con.i("flag") & ((1 << 9) | (1 << 2))))
                continue;
            if (con.i("type") != kCopyTransforms)
            {
                warnOnce("objectConstraint:" + on.object.idName(),
                         std::format("'{}': object constraints other than Copy Transforms are not applied", on.object.idName()));
                continue;
            }
            const View settings = con.deref("data");
            const View target = settings ? settings.deref("tar") : View();
            auto it = target ? objects.find(target.data()) : objects.end();
            if (it == objects.end())
                continue;
            NodeId from = it->second.id;
            if (const std::string bone = settings.str("subtarget"); !bone.empty())
                if (auto jt = it->second.joints.find(bone); jt != it->second.joints.end())
                    from = jt->second;
            if (!evaluated)
            {
                g.evaluate();
                evaluated = true;
            }
            // The node's own matrix (its rotate axis aside) takes the target's world, relative to its parent.
            const glm::dmat4 targetWorld = g.node(from)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            const NodeId parent = g.parent(on.id);
            const glm::dmat4 parentWorld = parent != kNoNode ? g.node(parent)->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
            const fbx::Trs trs = fbx::decompose(glm::inverse(parentWorld) * targetWorld);
            g.setValue({on.id, Transform::kRotateOrder, -1}, int64_t(scene::RotateOrder::XYZ));
            g.setValue({on.id, Transform::kTranslate, -1}, trs.t);
            g.setValue({on.id, Transform::kRotate, -1}, trs.r);
            g.setValue({on.id, Transform::kScale, -1}, trs.s);
            constrainedObjects.insert(data);
            if (evaluated)
                g.evaluate();
        }
}

void Importer::buildSkins()
{
    LILA_ZONE();

    if (skinJobs.empty() || !scene::types::find("skinCluster"))
        return;
    g.evaluate();
    for (const SkinJob& job : skinJobs)
    {
        auto armIt = objects.find(job.armature.data());
        if (armIt == objects.end())
        {
            r.warnings.push_back(std::format("'{}': its armature '{}' is not in the scene; imported unskinned", job.object.idName(), job.armature.idName()));
            continue;
        }
        const ObjectNode& arm = armIt->second;
        const View me = job.meshData;
        const size_t nV = size_t(std::max(0, me.i("totvert")));

        // The deform vertices: an MDeformVert per point.
        const Block* dvBlock = nullptr;
        int32_t dvScope = -1;
        for (const View& layer : me.sub("vdata").array("layers", size_t(std::max(0, me.sub("vdata").i("totlayer"))), "CustomDataLayer"))
            if (layer.i("type") == 2)
            {
                dvBlock = layer.block("data");
                dvScope = dvBlock ? dvBlock->owner : -1;
            }
        if (!dvBlock && me.ptr("dvert"))
        {
            dvBlock = me.block("dvert");
            dvScope = dvBlock ? dvBlock->owner : -1;
        }
        const blend::Struct* mdv = file.type("MDeformVert");
        if (!dvBlock || !mdv || dvBlock->size < nV * mdv->size)
        {
            r.warnings.push_back(std::format("'{}': deforms with '{}' but has no vertex weights; imported unskinned", job.object.idName(), job.armature.idName()));
            continue;
        }
        std::vector<View> groups = me.list("vertex_group_names", "bDeformGroup");
        if (groups.empty())
            groups = job.object.list("defbase", "bDeformGroup");

        const NodeId meshId = job.mesh;
        // The skin moves the points with the joints, so a mesh under the armature would move twice: it goes beside it.
        for (NodeId p = g.parent(meshId); p != kNoNode; p = g.parent(p))
            if (p == arm.id)
            {
                g.setParent(meshId, g.parent(arm.id), true);
                g.evaluate();
                break;
            }
        const glm::dmat4 W = g.node(meshId)->as<glm::dmat4>(Transform::kWorldMatrix);
        scene::Node* sc = g.createNode(*scene::types::find("skinCluster"), g.node(meshId)->name() + "Skin", kNoNode, false);
        const NodeId skinId = sc->id();
        scene::Node* source = g.createNode(*scene::types::find("meshSource"), "meshSource", kNoNode, true);
        const scene::PlugRef top{meshId, scene::MeshNode::kInMesh, -1};
        g.setValue({source->id(), scene::MeshSourceNode::kMesh, -1}, g.storedValue(top));
        g.setValue(top, scene::MeshPtr());
        g.connect({source->id(), scene::MeshSourceNode::kOutMesh, -1}, {skinId, scene::SkinClusterNode::kInMesh, -1});
        g.connect({skinId, scene::SkinClusterNode::kOutMesh, -1}, top);
        g.setValue({skinId, scene::SkinClusterNode::kGeomMatrix}, W);

        std::vector<int64_t> influenceOf(groups.size(), -1);
        for (size_t gi = 0; gi < groups.size(); ++gi)
        {
            const std::string name = groups[gi].str("name");
            auto jt = arm.joints.find(name);
            auto rt = arm.rest.find(name);
            if (jt == arm.joints.end() || (rt != arm.rest.end() && !rt->second.deform))
                continue;
            const uint32_t index = scene::skin::addInfluence(g, skinId, jt->second);
            influenceOf[gi] = index;
            const glm::dmat4 jointWorld = g.node(jt->second)->as<glm::dmat4>(Transform::kWorldMatrix);
            for (const scene::skin::Influence& inf : scene::skin::influences(g, skinId))
                if (inf.index == index)
                    g.setValue({inf.link, scene::SkinInfluenceNode::kBindPreMatrix, -1}, glm::inverse(jointWorld));
        }

        scene::WeightTable t;
        t.points.resize(nV);
        size_t maxUsed = 0;
        for (size_t p = 0; p < nV; ++p)
        {
            const View dv(&file, mdv, dvBlock->data + p * mdv->size, dvScope);
            std::map<uint32_t, float> sum;
            for (const View& w : dv.array("dw", size_t(std::max(0, dv.i("totweight"))), "MDeformWeight"))
            {
                const int nr = w.i("def_nr");
                const float weight = w.f("weight");
                if (nr >= 0 && size_t(nr) < influenceOf.size() && influenceOf[size_t(nr)] >= 0 && weight > 0.0f)
                    sum[uint32_t(influenceOf[size_t(nr)])] += weight;
            }
            float total = 0.0f;
            for (const auto& [inf, w] : sum)
                total += w;
            for (const auto& [inf, w] : sum)
                t.points[p].push_back({inf, total > 0.0f ? w / total : w});
            maxUsed = std::max(maxUsed, t.points[p].size());
        }
        g.setValue({skinId, scene::SkinClusterNode::kMaxInfluences}, int64_t(std::max<size_t>(4, maxUsed)));
        scene::skin::setWeights(g, skinId, t);
        r.skins.push_back(skinId);
    }
}

void Importer::buildMorphs()
{
    LILA_ZONE();

    if (morphJobs.empty() || !scene::types::find("morph"))
        return;
    for (MorphJob& job : morphJobs)
    {
        const View key = job.meshData.deref("key");
        if (!key)
            continue;
        const std::vector<View> blocks = key.list("block", "KeyBlock");
        if (blocks.size() < 2)
            continue;
        if (key.i("type") == 0)
        {
            r.skipped.push_back(std::format("mesh '{}': absolute shape keys", job.meshData.idName()));
            continue;
        }
        const View basis = key.deref("refkey") ? key.deref("refkey") : blocks.front();
        const size_t nV = size_t(std::max(0, job.meshData.i("totvert")));
        auto positions = [&](const View& kb) -> std::span<const float> {
            std::span<const float> d = kb.raw<float>("data");
            return d.size() >= nV * 3 ? d.first(nV * 3) : std::span<const float>();
        };
        const NodeId morph = scene::morphs::create(g, job.mesh, "Morph");
        r.morphs.push_back(morph);
        for (size_t b = 0; b < blocks.size(); ++b)
        {
            const View& kb = blocks[b];
            if (kb.data() == basis.data())
                continue;
            const int rel = kb.i("relative");
            const View relative = rel >= 0 && size_t(rel) < blocks.size() ? blocks[size_t(rel)] : basis;
            const std::span<const float> shape = positions(kb), ref = positions(relative);
            if (shape.empty() || ref.empty())
                continue;
            auto deltas = std::make_shared<scene::MorphDeltas>();
            for (size_t p = 0; p < nV; ++p)
            {
                const glm::dvec3 d(shape[p * 3] - ref[p * 3], shape[p * 3 + 1] - ref[p * 3 + 1], shape[p * 3 + 2] - ref[p * 3 + 2]);
                if (d == glm::dvec3(0.0))
                    continue;
                deltas->indices.push_back(uint32_t(p));
                deltas->deltas.push_back(axes.direction(d) * axes.cm);
            }
            const std::string name = kb.str("name");
            const double weight = (kb.i("flag") & kKeyBlockMute) ? 0.0 : kb.num("curval");
            const NodeId target = scene::morphs::addTarget(g, morph, fbx::safeName(name, "target"), deltas, weight);
            if (job.on)
            {
                job.on->morphTargets.push_back(target);
                job.on->morphNames.push_back(name);
            }
            if (!kb.str("vgroup").empty())
                warnOnce("keyGroup:" + name, std::format("shape key '{}': its vertex group mask is not applied", name));
            if (rel >= 0 && relative.data() != basis.data())
                warnOnce("keyRelative:" + name, std::format("shape key '{}' is relative to '{}', not the basis; its deltas are taken against that key", name, relative.str("name")));
        }
    }
}

// Animation ------------------------------------------------------------------------------------------------------------

namespace
{
/** The F-Curves an ID's animation data drives it with: its slot in a layered action, or a legacy action's curves. */
std::vector<View> fcurvesOf(const View& adt)
{
    std::vector<View> out;
    const View act = adt ? adt.deref("action") : View();
    if (!act)
        return out;
    const int layers = act.i("layer_array_num");
    if (layers <= 0)
        return act.list("curves", "FCurve");
    const int handle = adt.i("slot_handle");
    const std::vector<View> datas = act.pointerArray("strip_keyframe_data_array", size_t(std::max(0, act.i("strip_keyframe_data_array_num"))));
    for (const View& layer : act.pointerArray("layer_array", size_t(layers)))
    {
        if (!layer)
            continue;
        for (const View& strip : layer.pointerArray("strip_array", size_t(std::max(0, layer.i("strip_array_num")))))
        {
            if (!strip || strip.i("strip_type") != 0)
                continue;
            const int index = strip.i("data_index");
            if (index < 0 || size_t(index) >= datas.size() || !datas[size_t(index)])
                continue;
            const View data = datas[size_t(index)];
            for (const View& bag : data.pointerArray("channelbag_array", size_t(std::max(0, data.i("channelbag_array_num")))))
                if (bag && bag.i("slot_handle") == handle)
                    for (const View& fc : bag.pointerArray("fcurve_array", size_t(std::max(0, bag.i("fcurve_array_num")))))
                        if (fc)
                            out.push_back(fc);
        }
        break; // the first layer: Blender's UI only makes one
    }
    return out;
}
} // namespace

void Importer::importAnimation()
{
    LILA_ZONE();

    std::set<std::string> unconverted;
    auto toCurve = [&](const View& fc, double scale, double offset) {
        anim::Curve c;
        c.weighted = true;
        const std::vector<View> keys = fc.array("bezt", size_t(std::max(0, fc.i("totvert"))), "BezTriple");
        std::vector<glm::dvec2> hl, k, hr;
        for (const View& bt : keys)
        {
            hl.emplace_back(bt.num("vec", 0), bt.num("vec", 1));
            k.emplace_back(bt.num("vec", 3), bt.num("vec", 4));
            hr.emplace_back(bt.num("vec", 6), bt.num("vec", 7));
        }
        for (size_t i = 0; i < keys.size(); ++i)
        {
            anim::Key key;
            key.time = k[i].x / fps;
            key.value = k[i].y * scale + offset;
            key.in = key.out = anim::Tangent::Fixed;
            const double dtIn = (k[i].x - hl[i].x) / fps, dtOut = (hr[i].x - k[i].x) / fps;
            const double segIn = i > 0 ? (k[i].x - k[i - 1].x) / fps : dtIn * 3.0;
            const double segOut = i + 1 < keys.size() ? (k[i + 1].x - k[i].x) / fps : dtOut * 3.0;
            if (dtIn > 1e-9 && segIn > 1e-9)
            {
                key.inAngle = glm::degrees(std::atan2((k[i].y - hl[i].y) * scale, dtIn));
                key.inWeight = dtIn / (segIn / 3.0);
            }
            else
                key.in = anim::Tangent::Auto;
            if (dtOut > 1e-9 && segOut > 1e-9)
            {
                key.outAngle = glm::degrees(std::atan2((hr[i].y - k[i].y) * scale, dtOut));
                key.outWeight = dtOut / (segOut / 3.0);
            }
            else
                key.out = anim::Tangent::Auto;
            const int ipo = keys[i].i("ipo");
            if (ipo == 0)
                key.out = anim::Tangent::Step;
            else if (ipo == 1)
                key.out = anim::Tangent::Linear;
            else if (ipo >= 3)
            {
                key.out = anim::Tangent::Auto;
                warnOnce("easing", "keys with Blender's easing interpolations come in as smooth spline keys");
            }
            if (i > 0 && keys[i - 1].i("ipo") == 1)
                key.in = anim::Tangent::Linear;
            c.keys.push_back(key);
        }
        if (fc.i("extend") == 1)
            c.pre = c.post = anim::Infinity::Linear;
        for (const View& mod : fc.list("modifiers", "FModifier"))
            if (mod.i("type") == 1)
                c.pre = c.post = anim::Infinity::Cycle;
        return c;
    };
    auto connect = [&](NodeId node, int attr, int component, const char* type, const anim::Curve& c, const std::string& suffix) {
        if (c.keys.empty())
            return;
        fbx::connectCurve(g, {node, attr, component}, type, c, g.node(node)->name() + "_" + suffix);
    };
    auto keyFrames = [&](const std::vector<View>& fcs) {
        std::set<double> frames;
        for (const View& fc : fcs)
            for (const View& bt : fc.array("bezt", size_t(std::max(0, fc.i("totvert"))), "BezTriple"))
                frames.insert(bt.num("vec", 3));
        return frames;
    };
    // Quaternion or axis-angle channels (4 curves, w first) as a rotation curve.
    auto rotationCurve = [&](NodeId node, const std::array<View, 4>& fcs, const View& fallback, bool axisAngle) {
        std::array<anim::Curve, 4> cs;
        std::vector<View> present;
        for (int i = 0; i < 4; ++i)
            if (fcs[size_t(i)])
            {
                cs[size_t(i)] = toCurve(fcs[size_t(i)], 1.0, 0.0);
                present.push_back(fcs[size_t(i)]);
            }
        anim::RotationCurve rc;
        rc.order = scene::RotateOrder::XYZ;
        const glm::dvec4 still = axisAngle ? glm::dvec4(fallback.num("rotAngle"), fallback.vec3("rotAxis", glm::dvec3(0, 1, 0))) : fallback.vec4("quat", glm::dvec4(1, 0, 0, 0));
        for (double frame : keyFrames(present))
        {
            glm::dvec4 v;
            for (int i = 0; i < 4; ++i)
                v[i] = fcs[size_t(i)] ? cs[size_t(i)].evaluate(frame / fps) : still[i];
            glm::dquat q;
            if (axisAngle)
            {
                const glm::dvec3 ax(v[1], v[2], v[3]);
                q = glm::length(ax) > 0.0 ? glm::angleAxis(v[0], glm::normalize(ax)) : glm::dquat(1, 0, 0, 0);
            }
            else
            {
                q = glm::dquat(v[0], v[1], v[2], v[3]);
                q = glm::length(q) > 0.0 ? glm::normalize(q) : glm::dquat(1, 0, 0, 0);
            }
            const glm::dquat lq = axes.quat(q);
            anim::RotKey key = rc.makeKey(frame / fps, scene::matrixToEuler(glm::mat3_cast(lq), rc.order));
            if (!rc.keys.empty())
                key.euler = scene::eulerNear(glm::mat3_cast(lq), rc.order, rc.keys.back().euler);
            rc.keys.push_back(key);
        }
        if (rc.keys.empty())
            return;
        g.setValue({node, Transform::kRotateOrder, -1}, int64_t(rc.order));
        fbx::connectRotationCurve(g, node, rc);
    };

    for (auto& [objectData, on] : objects)
    {
        const View object = on.object;
        // Object channels, and bone channels on armatures.
        struct Channels
        {
            std::array<View, 3> loc, eul, scale;
            std::array<View, 4> quat, axisAngle;
        };
        std::map<std::string, Channels> byTarget; // "" is the object
        for (const View& fc : fcurvesOf(object.deref("adt")))
        {
            const std::string raw = fc.cstr("rna_path");
            const Path p = parsePath(raw);
            const int index = fc.i("array_index");
            std::string target;
            if (p.collection == "pose.bones")
                target = p.key;
            else if (!p.collection.empty())
            {
                unconverted.insert(std::format("{}: {}", object.idName(), raw));
                continue;
            }
            Channels& ch = byTarget[target];
            if (p.property == "location" && index >= 0 && index < 3)
                ch.loc[size_t(index)] = fc;
            else if (p.property == "rotation_euler" && index >= 0 && index < 3)
                ch.eul[size_t(index)] = fc;
            else if (p.property == "scale" && index >= 0 && index < 3)
                ch.scale[size_t(index)] = fc;
            else if (p.property == "rotation_quaternion" && index >= 0 && index < 4)
                ch.quat[size_t(index)] = fc;
            else if (p.property == "rotation_axis_angle" && index >= 0 && index < 4)
                ch.axisAngle[size_t(index)] = fc;
            else
                unconverted.insert(std::format("{}: {}", object.idName(), raw));
        }
        // Constraints place bones from other bones in Blender. Lila doesn't run them, so keys on the rest of the rig would
        // pull it apart: the whole rig keeps the pose Blender saved, which includes every constraint.
        bool rigConstrained = false;
        if (const View pose = object.deref("pose"))
            for (const View& c : pose.list("chanbase", "bPoseChannel"))
                for (const View& con : c.list("constraints", "bConstraint"))
                    rigConstrained = rigConstrained || (con.num("enforce", 0, 1.0) > 0.0 && !(con.i("flag") & ((1 << 9) | (1 << 2))));
        if (rigConstrained && byTarget.size() > (byTarget.count("") ? 1u : 0u))
            r.skipped.push_back(std::format("'{}': bone animation (its bones are driven by constraints, so the rig keeps the pose of the saved frame)", object.idName()));
        for (const auto& [target, ch] : byTarget)
        {
            if (target.empty())
            {
                if (constrainedObjects.count(objectData))
                    continue;
                const NodeId node = on.self;
                const glm::dvec3 dloc = object.vec3("dloc");
                const glm::dvec3 dscale = object.has("dscale") ? object.vec3("dscale", glm::dvec3(1.0)) : glm::dvec3(1.0);
                for (int a = 0; a < 3; ++a)
                {
                    const int la = axes.axis[a];
                    const char* xyz[] = {"translateX", "translateY", "translateZ"};
                    const char* rxyz[] = {"rotateX", "rotateY", "rotateZ"};
                    const char* sxyz[] = {"scaleX", "scaleY", "scaleZ"};
                    if (ch.loc[size_t(a)])
                        connect(node, Transform::kTranslate, la, "animCurveTL", toCurve(ch.loc[size_t(a)], axes.cm * axes.sign[a], dloc[a] * axes.cm * axes.sign[a]), xyz[la]);
                    if (ch.eul[size_t(a)])
                        connect(node, Transform::kRotate, la, "animCurveTA", toCurve(ch.eul[size_t(a)], axes.sign[a] * 180.0 / std::numbers::pi, 0.0), rxyz[la]);
                    if (ch.scale[size_t(a)])
                        connect(node, Transform::kScale, la, "animCurveTU", toCurve(ch.scale[size_t(a)], dscale[a], 0.0), sxyz[la]);
                }
                if (ch.quat[0] || ch.quat[1] || ch.quat[2] || ch.quat[3])
                    rotationCurve(node, ch.quat, object, false);
                if (ch.axisAngle[0] || ch.axisAngle[1] || ch.axisAngle[2] || ch.axisAngle[3])
                    rotationCurve(node, ch.axisAngle, object, true);
                continue;
            }
            auto jt = on.joints.find(target);
            auto rt = on.rest.find(target);
            if (jt == on.joints.end() || rt == on.rest.end())
                continue;
            const NodeId joint = jt->second;
            const ObjectNode::Rest& rest = rt->second;
            View chan;
            if (const View pose = object.deref("pose"))
                for (const View& c : pose.list("chanbase", "bPoseChannel"))
                    if (c.str("name") == target)
                        chan = c;
            if (rigConstrained)
                continue;
            for (int a = 0; a < 3; ++a)
            {
                const int la = axes.axis[a];
                const char* rxyz[] = {"rotateX", "rotateY", "rotateZ"};
                const char* sxyz[] = {"scaleX", "scaleY", "scaleZ"};
                if (ch.eul[size_t(a)])
                    connect(joint, Transform::kRotate, la, "animCurveTA", toCurve(ch.eul[size_t(a)], axes.sign[a] * 180.0 / std::numbers::pi, 0.0), rxyz[la]);
                if (ch.scale[size_t(a)])
                    connect(joint, Transform::kScale, la, "animCurveTU", toCurve(ch.scale[size_t(a)], 1.0, 0.0), sxyz[la]);
            }
            if (chan && (ch.quat[0] || ch.quat[1] || ch.quat[2] || ch.quat[3]))
                rotationCurve(joint, ch.quat, chan, false);
            if (chan && (ch.axisAngle[0] || ch.axisAngle[1] || ch.axisAngle[2] || ch.axisAngle[3]))
                rotationCurve(joint, ch.axisAngle, chan, true);
            // Pose location is in the bone's rest frame: baked per frame onto the joint's translate.
            if (ch.loc[0] || ch.loc[1] || ch.loc[2])
            {
                std::array<anim::Curve, 3> src;
                std::vector<View> present;
                for (int a = 0; a < 3; ++a)
                    if (ch.loc[size_t(a)])
                    {
                        src[size_t(a)] = toCurve(ch.loc[size_t(a)], 1.0, 0.0);
                        present.push_back(ch.loc[size_t(a)]);
                    }
                const std::set<double> frames = keyFrames(present);
                const glm::dvec3 still = chan ? chan.vec3("loc") : glm::dvec3(0.0);
                std::array<anim::Curve, 3> out;
                for (double f = std::floor(*frames.begin()); f <= std::ceil(*frames.rbegin()) + 1e-9; f += 1.0)
                {
                    glm::dvec3 loc;
                    for (int a = 0; a < 3; ++a)
                        loc[a] = ch.loc[size_t(a)] ? src[size_t(a)].evaluate(f / fps) : still[a];
                    const glm::dvec3 t = rest.t + rest.r * axes.point(loc);
                    for (int c = 0; c < 3; ++c)
                    {
                        anim::Key key;
                        key.time = f / fps;
                        key.value = t[c];
                        out[size_t(c)].keys.push_back(key);
                    }
                }
                const char* xyz[] = {"translateX", "translateY", "translateZ"};
                for (int c = 0; c < 3; ++c)
                    connect(joint, Transform::kTranslate, c, "animCurveTL", out[size_t(c)], xyz[c]);
            }
        }

        // Shape key weights.
        if (on.type == ob::kMesh && !on.morphTargets.empty())
            if (const View key = object.deref("data").deref("key"))
                for (const View& fc : fcurvesOf(key.deref("adt")))
                {
                    const Path p = parsePath(fc.cstr("rna_path"));
                    if (p.collection != "key_blocks" || p.property != "value")
                        continue;
                    for (size_t t = 0; t < on.morphNames.size(); ++t)
                        if (on.morphNames[t] == p.key)
                            connect(on.morphTargets[t], scene::morphs::kWeight, -1, "animCurveTU", toCurve(fc, 1.0, 0.0), "weight");
                }

        // Camera lens and light power.
        const View data = object.deref("data");
        if (data && (on.type == ob::kCamera || on.type == ob::kLight))
            for (const View& fc : fcurvesOf(data.deref("adt")))
            {
                const std::string path = fc.cstr("rna_path");
                if (on.type == ob::kCamera && path == "lens")
                    connect(on.id, scene::Camera::kFocalLength, -1, "animCurveTU", toCurve(fc, 1.0, 0.0), "focalLength");
                else if (on.type == ob::kLight && path == "energy")
                    connect(on.id, scene::Light::kIntensity, -1, "animCurveTU", toCurve(fc, 1.0, 0.0), "intensity");
                else
                    unconverted.insert(std::format("{}: {}", data.idName(), path));
            }

        if (const View adt = object.deref("adt"))
        {
            if (!adt.list("drivers").empty())
                r.skipped.push_back(std::format("'{}': {} driver(s)", object.idName(), adt.list("drivers").size()));
            if (!adt.ptr("action") && !adt.list("nla_tracks").empty())
                r.skipped.push_back(std::format("'{}': NLA strips (only the active action is read)", object.idName()));
        }
    }
    for (const std::string& u : unconverted)
        r.skipped.push_back("animation of " + u);
}
} // namespace lila::blend
