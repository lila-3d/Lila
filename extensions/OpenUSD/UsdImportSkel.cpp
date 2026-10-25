// UsdSkel on import: Skeleton prims become joint hierarchies, SkelBindingAPI meshes skinClusters, BlendShapes morph
// targets with their in-betweens, and SkelAnimation keys on the joints and the target weights.

#include "UsdImport.h"

#include <lila/Anim.h>
#include <lila/Deformer.h>
#include <lila/Mesh.h>
#include <lila/MeshOps.h>
#include <lila/Node.h>
#include <lila/Rig.h>

#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdSkel/animQuery.h>
#include <pxr/usd/usdSkel/animation.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/blendShape.h>
#include <pxr/usd/usdSkel/inbetweenShape.h>
#include <pxr/usd/usdSkel/skeleton.h>
#include <pxr/usd/usdSkel/topology.h>

#include <algorithm>
#include <format>

PXR_NAMESPACE_USING_DIRECTIVE

namespace lila::usd
{
using scene::NodeId;
using scene::kNoNode;

namespace
{
/** Joint-local rest transforms: restTransforms, else derived from the bind pose. */
VtMatrix4dArray restLocals(const UsdSkelSkeleton& skel, const UsdSkelTopology& topo, size_t count, UsdTimeCode t)
{
    VtMatrix4dArray rest;
    skel.GetRestTransformsAttr().Get(&rest, t);
    if (rest.size() == count)
        return rest;
    VtMatrix4dArray bind;
    skel.GetBindTransformsAttr().Get(&bind, t);
    rest.assign(count, GfMatrix4d(1.0));
    if (bind.size() == count)
        for (size_t i = 0; i < count; ++i)
        {
            const int p = topo.GetParent(i);
            rest[i] = p >= 0 ? bind[i] * bind[size_t(p)].GetInverse() : bind[i];
        }
    return rest;
}

UsdSkelAnimation animationOf(const UsdPrim& skelPrim)
{
    UsdPrim anim = UsdSkelBindingAPI(skelPrim).GetInheritedAnimationSource();
    return anim ? UsdSkelAnimation(anim) : UsdSkelAnimation();
}

/** Joint locals at t: the rest pose with whatever the animation drives laid over it. */
VtMatrix4dArray posed(Importer& im, const VtTokenArray& joints, const VtMatrix4dArray& rest, const UsdSkelAnimQuery& aq, UsdTimeCode t)
{
    VtMatrix4dArray out = rest;
    if (!aq)
        return out;
    VtMatrix4dArray xf;
    if (!aq.ComputeJointLocalTransforms(&xf, t))
        return out;
    const VtTokenArray animJoints = aq.GetJointOrder();
    std::unordered_map<TfToken, size_t, TfToken::HashFunctor> index;
    for (size_t i = 0; i < joints.size(); ++i)
        index[joints[i]] = i;
    for (size_t k = 0; k < animJoints.size() && k < xf.size(); ++k)
        if (auto it = index.find(animJoints[k]); it != index.end())
            out[it->second] = xf[k];
    (void)im;
    return out;
}

std::string leafName(const TfToken& joint)
{
    const std::string& s = joint.GetString();
    const size_t slash = s.rfind('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}
} // namespace

void Importer::buildSkeleton(const UsdPrim& prim, NodeId node)
{
    LILA_ZONE();

    UsdSkelSkeleton skel(prim);
    Skel s;
    s.parent = node;
    s.synthetic = lilaFlag(prim, "synthetic");
    VtTokenArray joints;
    skel.GetJointsAttr().Get(&joints);
    const UsdSkelTopology topo(joints);
    if (std::string why; !topo.Validate(&why))
    {
        r.warnings.push_back(std::format("{}: invalid skeleton ({}); its joints were not imported", prim.GetPath().GetString(), why));
        skeletons[prim.GetPath()] = s;
        return;
    }
    if (s.synthetic)
    {
        skeletons[prim.GetPath()] = s;
        return;
    }
    const scene::NodeType* jointType = scene::types::find("joint");
    const VtMatrix4dArray rest = restLocals(skel, topo, joints.size(), restTime);
    UsdSkelAnimation anim = animationOf(prim);
    const VtMatrix4dArray pose = posed(*this, joints, rest, anim ? skelCache.GetAnimQuery(anim) : UsdSkelAnimQuery(), restTime);
    s.joints.assign(joints.size(), kNoNode);
    for (size_t i = 0; i < joints.size(); ++i)
    {
        const int p = topo.GetParent(i);
        const NodeId parent = p >= 0 ? s.joints[size_t(p)] : node;
        scene::Node* n = g.createNode(*jointType, identifier(leafName(joints[i]), "joint"), parent, false);
        s.joints[i] = n->id();
        r.nodes.push_back(n->id());
        r.joints.push_back(n->id());
        if (parent == opts.parent)
            r.roots.push_back(n->id());
        Trs trs = decompose(axes.matrixToLila(toGlm(pose[i])), scene::RotateOrder::XYZ, glm::dvec3(0.0));
        if (trs.sheared)
            r.warnings.push_back(std::format("{}: shear dropped from joint {}", prim.GetPath().GetString(), joints[i].GetString()));
        g.setValue({n->id(), scene::Transform::kTranslate, -1}, trs.t);
        g.setValue({n->id(), scene::Transform::kRotate, -1}, trs.r);
        g.setValue({n->id(), scene::Transform::kScale, -1}, trs.s);
    }
    skeletons[prim.GetPath()] = s;
}

void Importer::bindAll()
{
    LILA_ZONE();

    using namespace scene;
    if (bound.empty())
        return;
    g.evaluate();
    bool normalOffsetsWarned = false;
    for (const auto& [path, meshId] : bound)
    {
        const UsdPrim prim = stage->GetPrimAtPath(path);
        if (!prim)
            continue;
        const std::string where = prim.GetPath().GetString();
        UsdSkelBindingAPI binding(prim);
        UsdSkelSkeleton skel = binding.GetInheritedSkeleton();
        if (!skel)
        {
            r.warnings.push_back(std::format("{}: skinned but bound to no skeleton; imported unskinned", where));
            continue;
        }
        auto found = skeletons.find(skel.GetPath());
        if (found == skeletons.end())
        {
            r.warnings.push_back(std::format("{}: its skeleton {} was not imported; imported unskinned", where, skel.GetPath().GetString()));
            continue;
        }
        const Skel& s = found->second;
        const std::vector<uint32_t>& pointOf = pointMaps[prim.GetPath()];
        const glm::dmat4 skelWorld = s.parent != kNoNode ? g.node(s.parent)->as<glm::dmat4>(Transform::kWorldMatrix) : glm::dmat4(1.0);
        const glm::dmat4 meshWorld = g.node(meshId)->as<glm::dmat4>(Transform::kWorldMatrix);

        GfMatrix4d geomBind(1.0);
        if (UsdGeomPrimvar gb = UsdGeomPrimvarsAPI(prim).FindPrimvarWithInheritance(TfToken("skel:geomBindTransform")); gb)
            gb.Get(&geomBind, restTime);
        const glm::dmat4 geomWorld = skelWorld * axes.matrixToLila(toGlm(geomBind));
        // UsdSkel skins points in skeleton space and ignores the mesh's own transform; Lila skins in the mesh's space at
        // bind time, so points carry any difference between the two.
        const glm::dmat4 toMesh = glm::inverse(meshWorld) * geomWorld;
        const bool moved = !nearMatrix(toMesh, glm::dmat4(1.0), 1e-9);
        const PlugRef top{meshId, MeshNode::kInMesh, -1};
        if (moved)
            if (MeshPtr m = std::get<MeshPtr>(g.storedValue(top)))
            {
                auto copy = std::make_shared<geo::Mesh>(*m);
                for (geo::Index p = 0; p < copy->pointCapacity(); ++p)
                    if (copy->pointValid(p))
                        copy->setPosition(p, glm::dvec3(toMesh * glm::dvec4(copy->position(p), 1.0)));
                if (copy->cornerAttrs().has(geo::attr::kNormal))
                {
                    const glm::dmat3 nm = glm::inverse(glm::transpose(glm::dmat3(toMesh)));
                    for (glm::vec3& n : copy->cornerAttrs().get<glm::vec3>(geo::attr::kNormal))
                        n = glm::vec3(glm::normalize(nm * glm::dvec3(n)));
                }
                g.setValue(top, MeshPtr(std::move(copy)));
            }

        // Skin.
        UsdGeomPrimvar ji = binding.GetJointIndicesPrimvar(), jw = binding.GetJointWeightsPrimvar();
        const bool hasSkin = ji && jw && ji.HasValue() && jw.HasValue() && !s.synthetic;
        if (hasSkin && !opts.skins)
            r.skipped.push_back(std::format("skin on '{}' (imported unskinned)", where));
        if (hasSkin && opts.skins && types::find("skinCluster"))
        {
            VtIntArray indices;
            VtFloatArray weights;
            ji.ComputeFlattened(&indices, restTime);
            jw.ComputeFlattened(&weights, restTime);
            const int per = std::max(1, ji.GetElementSize());
            const bool rigid = ji.GetInterpolation() == UsdGeomTokens->constant;
            VtTokenArray skelJoints, meshJoints;
            skel.GetJointsAttr().Get(&skelJoints);
            binding.GetJointsAttr().Get(&meshJoints);
            std::unordered_map<TfToken, size_t, TfToken::HashFunctor> skelIndex;
            for (size_t i = 0; i < skelJoints.size(); ++i)
                skelIndex[skelJoints[i]] = i;
            auto skelJoint = [&](int local) -> int {
                if (local < 0)
                    return -1;
                if (meshJoints.empty())
                    return size_t(local) < skelJoints.size() ? local : -1;
                if (size_t(local) >= meshJoints.size())
                    return -1;
                auto it = skelIndex.find(meshJoints[size_t(local)]);
                return it != skelIndex.end() ? int(it->second) : -1;
            };
            VtMatrix4dArray bind;
            skel.GetBindTransformsAttr().Get(&bind, restTime);
            if (bind.size() != skelJoints.size())
            {
                r.warnings.push_back(std::format("{}: skeleton {} has no bind pose; its rest pose stands in", where, skel.GetPath().GetString()));
                bind.assign(skelJoints.size(), GfMatrix4d(1.0));
                const UsdSkelTopology topo(skelJoints);
                const VtMatrix4dArray rest = restLocals(skel, topo, skelJoints.size(), restTime);
                for (size_t i = 0; i < rest.size(); ++i)
                    bind[i] = topo.GetParent(i) >= 0 ? rest[i] * bind[size_t(topo.GetParent(i))] : rest[i];
            }

            Node* sc = g.createNode(*types::find("skinCluster"), "SkinCluster", kNoNode, true);
            const NodeId skinId = sc->id();
            Node* source = g.createNode(*types::find("meshSource"), "meshSource", kNoNode, true);
            g.setValue({source->id(), MeshSourceNode::kMesh, -1}, g.storedValue(top));
            g.setValue(top, MeshPtr());
            g.connect({source->id(), MeshSourceNode::kOutMesh, -1}, {skinId, SkinClusterNode::kInMesh, -1});
            g.connect({skinId, SkinClusterNode::kOutMesh, -1}, top);
            g.setValue({skinId, SkinClusterNode::kGeomMatrix, -1}, meshWorld);

            MeshPtr bindMesh = skin::inputMesh(g, skinId);
            WeightTable table;
            table.points.resize(bindMesh ? bindMesh->pointCapacity() : 0);
            std::unordered_map<int, uint32_t> influenceOf;
            auto influence = [&](int joint) -> std::optional<uint32_t> {
                if (auto it = influenceOf.find(joint); it != influenceOf.end())
                    return it->second;
                if (joint < 0 || size_t(joint) >= s.joints.size() || s.joints[size_t(joint)] == kNoNode)
                    return std::nullopt;
                const uint32_t idx = skin::addInfluence(g, skinId, s.joints[size_t(joint)]);
                const glm::dmat4 bindWorld = skelWorld * axes.matrixToLila(toGlm(bind[size_t(joint)]));
                for (const skin::Influence& inf : skin::influences(g, skinId))
                    if (inf.index == idx)
                        g.setValue({inf.link, SkinInfluenceNode::kBindPreMatrix, -1}, glm::inverse(bindWorld));
                influenceOf[joint] = idx;
                return idx;
            };
            size_t dropped = 0;
            for (size_t p = 0; p < pointOf.size(); ++p)
            {
                const uint32_t lp = pointOf[p];
                if (lp == geo::kInvalid || lp >= table.points.size())
                    continue;
                const size_t base = rigid ? 0 : p * size_t(per);
                for (int k = 0; k < per; ++k)
                {
                    if (base + size_t(k) >= indices.size() || base + size_t(k) >= weights.size())
                        break;
                    const float w = weights[base + size_t(k)];
                    if (w <= 0.0f)
                        continue;
                    if (std::optional<uint32_t> inf = influence(skelJoint(indices[base + size_t(k)])))
                        table.points[lp].push_back({*inf, w});
                    else
                        ++dropped;
                }
            }
            if (dropped)
                r.warnings.push_back(std::format("{}: {} weight(s) name joints the skeleton doesn't have", where, dropped));
            g.setValue({skinId, SkinClusterNode::kMaxInfluences, -1}, int64_t(std::max(4, per)));
            skin::setWeights(g, skinId, table);
            r.skins.push_back(skinId);
        }

        // Blend shapes, before the skin in the stack like Unreal's morph targets.
        VtTokenArray shapeNames;
        SdfPathVector shapeTargets;
        binding.GetBlendShapesAttr().Get(&shapeNames);
        binding.GetBlendShapeTargetsRel().GetTargets(&shapeTargets);
        if (shapeNames.empty() || shapeTargets.empty())
            continue;
        if (shapeNames.size() != shapeTargets.size())
            r.warnings.push_back(std::format("{}: {} blend shape names for {} targets; the extra ones are left out", where, shapeNames.size(),
                                             shapeTargets.size()));
        const glm::dmat3 lin = glm::dmat3(toMesh) * glm::dmat3(axes.toLila);
        auto deltasOf = [&](const VtVec3fArray& offsets, const VtIntArray& indices) {
            auto d = std::make_shared<scene::MorphDeltas>();
            for (size_t k = 0; k < offsets.size(); ++k)
            {
                if (!indices.empty() && k >= indices.size())
                    break;
                const int raw = indices.empty() ? int(k) : indices[k];
                const size_t usdPoint = size_t(std::max(0, raw));
                if (raw < 0 || usdPoint >= pointOf.size() || pointOf[usdPoint] == geo::kInvalid)
                    continue;
                const GfVec3f& o = offsets[k];
                d->indices.push_back(pointOf[usdPoint]);
                d->deltas.push_back(lin * glm::dvec3(o[0], o[1], o[2]));
            }
            return d;
        };
        NodeId morph = scene::morphs::create(g, meshId, "Morph");
        Shapes entry;
        entry.skeleton = skel.GetPath();
        for (size_t i = 0; i < shapeNames.size() && i < shapeTargets.size(); ++i)
        {
            UsdSkelBlendShape bs(stage->GetPrimAtPath(shapeTargets[i]));
            if (!bs)
            {
                r.warnings.push_back(std::format("{}: blend shape {} has no target prim", where, shapeNames[i].GetString()));
                continue;
            }
            VtVec3fArray offsets, normalOffsets;
            VtIntArray indices;
            bs.GetOffsetsAttr().Get(&offsets);
            bs.GetPointIndicesAttr().Get(&indices);
            if (bs.GetNormalOffsetsAttr().Get(&normalOffsets) && !normalOffsets.empty() && !normalOffsetsWarned)
            {
                r.warnings.push_back("blend shape normal offsets are not kept; Lila recomputes normals from the shape");
                normalOffsetsWarned = true;
            }
            const NodeId target = scene::morphs::addTarget(g, morph, shapeNames[i].GetString(), deltasOf(offsets, indices), 0.0);
            for (const UsdSkelInbetweenShape& ib : bs.GetInbetweens())
            {
                float w = 0.0f;
                VtVec3fArray ibOffsets;
                if (!ib.GetWeight(&w) || !ib.GetOffsets(&ibOffsets))
                    continue;
                if (w <= 0.0f || w >= 1.0f)
                {
                    r.warnings.push_back(std::format("{}: in-between of {} at weight {} is outside 0 to 1; left out", where, shapeNames[i].GetString(), w));
                    continue;
                }
                scene::morphs::setInBetween(g, target, double(w), deltasOf(ibOffsets, indices));
            }
            entry.targets.push_back({shapeNames[i].GetString(), target});
        }
        shapes.push_back(std::move(entry));
    }
}

void Importer::keySkeletons()
{
    LILA_ZONE();

    for (auto& [path, s] : skeletons)
    {
        const UsdPrim skelPrim = stage->GetPrimAtPath(path);
        if (!skelPrim)
            continue;
        UsdSkelSkeleton skel(skelPrim);
        UsdSkelAnimation anim = animationOf(skelPrim);
        if (!anim)
            continue;
        UsdSkelAnimQuery aq = skelCache.GetAnimQuery(anim);
        if (!aq)
            continue;

        if (!s.synthetic && !s.joints.empty())
        {
            std::vector<double> samples;
            aq.GetJointTransformTimeSamples(&samples);
            if (samples.size() > 1)
            {
                VtTokenArray joints;
                skel.GetJointsAttr().Get(&joints);
                const UsdSkelTopology topo(joints);
                const VtMatrix4dArray rest = restLocals(skel, topo, joints.size(), restTime);
                const size_t n = s.joints.size();
                std::vector<std::vector<glm::dvec3>> t(n), rr(n), sc(n);
                std::vector<glm::dvec3> ref(n);
                for (size_t j = 0; j < n; ++j)
                    ref[j] = g.node(s.joints[j])->as<glm::dvec3>(scene::Transform::kRotate);
                std::vector<double> times;
                for (double tc : samples)
                {
                    const VtMatrix4dArray pose = posed(*this, joints, rest, aq, UsdTimeCode(tc));
                    times.push_back(seconds(tc));
                    for (size_t j = 0; j < n && j < pose.size(); ++j)
                    {
                        Trs trs = decompose(axes.matrixToLila(toGlm(pose[j])), scene::RotateOrder::XYZ, ref[j]);
                        ref[j] = trs.r;
                        t[j].push_back(trs.t);
                        rr[j].push_back(trs.r);
                        sc[j].push_back(trs.s);
                    }
                }
                for (size_t j = 0; j < n; ++j)
                    if (t[j].size() == times.size())
                        keyChannels(*this, s.joints[j], times, t[j], rr[j], sc[j]);
            }
        }

        // Blend shape weights, for every mesh bound to this skeleton.
        const VtTokenArray order = aq.GetBlendShapeOrder();
        if (order.empty())
            continue;
        std::unordered_map<std::string, size_t> channel;
        for (size_t i = 0; i < order.size(); ++i)
            channel[order[i].GetString()] = i;
        std::vector<double> samples;
        aq.GetBlendShapeWeightTimeSamples(&samples);
        VtFloatArray restWeights;
        aq.ComputeBlendShapeWeights(&restWeights, restTime);
        for (const Shapes& sh : shapes)
        {
            if (sh.skeleton != path)
                continue;
            for (const auto& [name, target] : sh.targets)
            {
                auto it = channel.find(name);
                if (it == channel.end())
                    continue;
                const scene::PlugRef plug{target, scene::morphs::kWeight, -1};
                if (it->second < restWeights.size())
                    g.setValue(plug, double(restWeights[it->second]));
                if (samples.size() < 2)
                    continue;
                std::vector<double> times, values;
                for (double tc : samples)
                {
                    VtFloatArray w;
                    aq.ComputeBlendShapeWeights(&w, UsdTimeCode(tc));
                    times.push_back(seconds(tc));
                    values.push_back(it->second < w.size() ? double(w[it->second]) : 0.0);
                }
                keyPlug(*this, plug, times, values);
            }
        }
    }
}
} // namespace lila::usd
