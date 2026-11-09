// The bendBones deformer: a mesh bound to chains of joints bends along the smooth curve through each chain (and its bone
// CVs), deform/BendBones.h doing the maths. Joints and CVs feed it through matrix array inputs, so posing them re-evaluates
// it; the rest pose and the binding are stored on it.

#include "scene/BendBonesDeformer.h"

#include "deform/BendBones.h"
#include "ops/Command.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Relocate.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <map>
#include <mutex>

namespace lila::scene::bendbones
{
using namespace rigutil;
namespace bb = lila::deform::bendbones;

namespace
{
constexpr const char* kType = "bendBones";
constexpr const char* kCvType = "bendBoneCV";

/** What a bind fixed: chain sizes, the joints' rest matrices, the bone CVs' rest places, and the binding. */
struct Bound
{
    std::vector<int> sizes;
    std::vector<glm::dmat4> rest;      // per joint, chains in order
    std::vector<int> cvBone;           // per CV: the global joint index of the bone's first joint
    std::vector<glm::dvec3> cvRest;    // per CV: rest position in that joint's space
    std::vector<glm::dvec3> normals;   // per joint
    glm::dmat4 meshWorld{1.0};          // the mesh's world matrix at bind: chains are in world, points in the mesh's space
    bb::Binding binding;
};

json boundJson(const Bound& b)
{
    json places = json::array();
    for (const bb::Place& p : b.binding.place)
        places.push_back({p.segment, p.fraction, p.offset.x, p.offset.y, p.offset.z});
    json cvRest = json::array(), normals = json::array();
    for (const glm::dvec3& v : b.cvRest)
        cvRest.push_back({v.x, v.y, v.z});
    for (const glm::dvec3& v : b.normals)
        normals.push_back({v.x, v.y, v.z});
    return {{"sizes", b.sizes}, {"cvBone", b.cvBone}, {"cvRest", cvRest}, {"normals", normals},
            {"offsets", b.binding.offsets}, {"chain", b.binding.chain}, {"weight", b.binding.weight}, {"place", places}};
}

Bound boundOf(const json& j)
{
    Bound b;
    b.sizes = j.value("sizes", std::vector<int>{});
    b.cvBone = j.value("cvBone", std::vector<int>{});
    for (const json& v : j.value("cvRest", json::array()))
        b.cvRest.push_back({v[0].get<double>(), v[1].get<double>(), v[2].get<double>()});
    for (const json& v : j.value("normals", json::array()))
        b.normals.push_back({v[0].get<double>(), v[1].get<double>(), v[2].get<double>()});
    b.binding.offsets = j.value("offsets", std::vector<uint32_t>{0});
    b.binding.chain = j.value("chain", std::vector<uint32_t>{});
    b.binding.weight = j.value("weight", std::vector<double>{});
    for (const json& p : j.value("place", json::array()))
        b.binding.place.push_back({.segment = p[0].get<int>(), .fraction = p[1].get<double>(), .offset = {p[2].get<double>(), p[3].get<double>(), p[4].get<double>()}});
    return b;
}

/** The chains from joint matrices and CV positions (world), split by the bind's sizes. */
std::vector<bb::Chain> chainsOf(const Bound& b, const std::vector<glm::dmat4>& joints, const std::vector<glm::dvec3>& cvs)
{
    std::vector<bb::Chain> out;
    size_t at = 0;
    std::vector<int> first;
    for (int n : b.sizes)
    {
        bb::Chain c;
        first.push_back(int(at));
        for (int k = 0; k < n && at < joints.size(); ++k, ++at)
        {
            c.position.push_back(glm::dvec3(joints[at][3]));
            c.rotation.push_back(orthonormal(glm::dmat3(joints[at])));
        }
        c.cvs.assign(c.position.size(), {});
        out.push_back(std::move(c));
    }
    for (size_t i = 0; i < cvs.size() && i < b.cvBone.size(); ++i)
        for (size_t ci = 0; ci < out.size(); ++ci)
            if (const int bone = b.cvBone[i] - first[ci]; bone >= 0 && bone + 1 < int(out[ci].position.size()))
            {
                out[ci].cvs[size_t(bone)].push_back(cvs[i]);
                break;
            }
    // CVs along a bone go in order of how far along it they sit.
    for (bb::Chain& c : out)
        for (size_t bone = 0; bone + 1 < c.position.size(); ++bone)
        {
            const glm::dvec3 a = c.position[bone], d = c.position[bone + 1] - a;
            std::sort(c.cvs[bone].begin(), c.cvs[bone].end(),
                      [&](const glm::dvec3& x, const glm::dvec3& y) { return glm::dot(x - a, d) < glm::dot(y - a, d); });
        }
    return out;
}

class BendBonesNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        Bound b = bound();
        // The world-space parts of the bind live in matrix attrs, which relocating the scene moves.
        for (const auto& [i, v] : elements(type().attrIndex("restMatrix")))
            b.rest.push_back(std::get<glm::dmat4>(v));
        b.meshWorld = as<glm::dmat4>(type().attrIndex("bindMeshMatrix"));
        if (b.sizes.empty() || b.binding.offsets.size() != points.size() + 1)
            return;
        std::vector<glm::dmat4> joints;
        for (const auto& [i, v] : elements(type().attrIndex("matrix")))
            joints.push_back(std::get<glm::dmat4>(v));
        std::vector<glm::dvec3> cvs, cvRest;
        for (const auto& [i, v] : elements(type().attrIndex("cvMatrix")))
            cvs.push_back(glm::dvec3(std::get<glm::dmat4>(v)[3]));
        if (joints.size() != b.rest.size())
            return;
        for (size_t i = 0; i < b.cvRest.size() && i < b.cvBone.size(); ++i)
            cvRest.push_back(glm::dvec3(b.rest[size_t(b.cvBone[i])] * glm::dvec4(b.cvRest[i], 1.0)));
        cvs.resize(std::min(cvs.size(), cvRest.size()));
        cvRest.resize(cvs.size());
        std::vector<bb::Curve> rest, posed;
        size_t at = 0;
        const std::vector<bb::Chain> restChains = chainsOf(b, b.rest, cvRest), posedChains = chainsOf(b, joints, cvs);
        for (size_t c = 0; c < restChains.size(); ++c)
        {
            const std::vector<glm::dvec3> normals(b.normals.begin() + long(at), b.normals.begin() + long(at + restChains[c].position.size()));
            at += restChains[c].position.size();
            rest.push_back(bb::build(restChains[c], normals));
            posed.push_back(bb::build(posedChains[c], normals));
        }
        const bb::Options o{.volume = as<double>(type().attrIndex("volume")), .evenBend = as<double>(type().attrIndex("evenBend"))};
        const glm::dmat4 toLocal = glm::inverse(b.meshWorld);
        bb::deformPoints(rest, posed, b.binding, o, points);
        for (glm::dvec3& p : points)
            p = glm::dvec3(toLocal * glm::dvec4(p, 1.0));
    }

private:
    /** The stored bind, parsed once per change of its text. */
    const Bound& bound() const
    {
        const std::string& text = as<std::string>(type().attrIndex("binding"));
        std::lock_guard lock(mMutex);
        if (text != mText)
        {
            mText = text;
            mBound = text.empty() ? Bound{} : boundOf(json::parse(text, nullptr, false));
        }
        return mBound;
    }

    mutable std::mutex mMutex;
    mutable std::string mText;
    mutable Bound mBound;
};

AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, uint32_t flags)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = flags, .doc = std::move(doc)};
}

/** A joint path, or the error a command reports. */
NodeId jointArg(Graph& g, const json& name, const char* command)
{
    Node* n = g.find(name.get<std::string>());
    if (!n || !rig::isJoint(*n))
        throw CommandError(std::string(command) + ": " + name.get<std::string>() + " is not a joint");
    return n->id();
}
} // namespace

void registerTypes()
{
    if (types::find(kType))
        return;
    const uint32_t keyable = AttrInput | AttrStorable | AttrKeyable;
    deformers::registerDeformer(
        {kType, "vector-spline",
         "Bends a mesh along the smooth curve through chains of joints (bend bones): each chain is one influence, the "
         "inside of a bend compresses evenly instead of pinching, stretching thins the mesh to keep its volume, and bone "
         "CVs reshape a bone without rebinding. Bake it to joints for Unreal.",
         {param("volume", PlugType::Float, 1.0, "0 keeps the mesh's thickness as chains stretch, 1 thins it to keep the volume.", keyable),
          param("evenBend", PlugType::Float, 1.0, "0 rides the curve, 1 spreads the inside and outside of a bend evenly.", keyable),
          param("matrix", PlugType::Matrix, glm::dmat4(1.0), "World matrices of the chains' joints, chains in order.", AttrInput | AttrHidden | AttrArray),
          param("cvMatrix", PlugType::Matrix, glm::dmat4(1.0), "World matrices of the bone CVs.", AttrInput | AttrHidden | AttrArray),
          param("binding", PlugType::String, std::string(), "The bind: chains, bone CVs and where each point sits.", AttrInput | AttrStorable | AttrHidden),
          param("restMatrix", PlugType::Matrix, glm::dmat4(1.0), "World matrices of the chains' joints at bind.", AttrInput | AttrStorable | AttrHidden | AttrArray),
          param("bindMeshMatrix", PlugType::Matrix, glm::dmat4(1.0), "The mesh's world matrix at bind.", AttrInput | AttrStorable | AttrHidden)},
         [] { return std::make_unique<BendBonesNode>(); }});

    NodeType cv{.name = kCvType,
                .base = types::find("transform"),
                .dag = true,
                .doc = "A bone CV of a bend bones chain: an extra point the bone's curve passes through. Move or key it to "
                       "reshape the bone; nothing is rebound.",
                .icon = "point",
                .create = types::find("transform")->create};
    types::registerType(std::move(cv));
    relocate::registerWorldAttrs(kType, {.matrices = {"restMatrix", "bindMeshMatrix"}});
    relocate::registerNeutral(kCvType);
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "bendBones",
        .doc = "Bind a mesh to chains of joints as bend bones: it bends along the smooth curve through each chain, each "
               "chain one influence, the joins between chains blending. Binds in the current pose. Returns the deformer.",
        .positionalDoc = "mesh",
        .flags = {
            {"chains", "ch", T::List, nullptr, "Chains, each a list of joints from top to end ([[thigh, calf, foot], ...])."},
            {"volume", "v", T::Float, 1.0, "Thin the mesh as chains stretch to keep its volume.", {}, {}, 0.0, 1.0},
            {"evenBend", "eb", T::Float, 1.0, "Spread the inside and outside of bends evenly.", {}, {}, 0.0, 1.0},
            {"name", "n", T::String, nullptr, "Name for the deformer."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("bendBones: give a mesh");
            Node& mesh = nodeNamed(objs.front(), "bendBones");
            if (!mesh.type().isA("mesh"))
                throw CommandError("bendBones: " + mesh.path() + " is not a mesh");
            if (!a.has("chains") || !a.flags["chains"].is_array() || a.flags["chains"].empty())
                throw CommandError("bendBones: give chains, each a list of joints from top to end");
            g.evaluate();
            Bound b;
            std::vector<NodeId> joints;
            std::vector<bb::Curve> rest;
            for (const json& chain : a.flags["chains"])
            {
                if (!chain.is_array() || chain.size() < 2)
                    throw CommandError("bendBones: a chain needs two joints or more");
                bb::Chain c;
                for (const json& j : chain)
                {
                    const NodeId id = jointArg(g, j, "bendBones");
                    joints.push_back(id);
                    const glm::dmat4 w = worldOf(*g.node(id));
                    b.rest.push_back(w);
                    c.position.push_back(glm::dvec3(w[3]));
                    c.rotation.push_back(orthonormal(glm::dmat3(w)));
                }
                b.sizes.push_back(int(chain.size()));
                const bb::Curve first = bb::build(c);
                const std::vector<glm::dvec3> normals = bb::restNormals(c, first);
                b.normals.insert(b.normals.end(), normals.begin(), normals.end());
                rest.push_back(bb::build(c, normals));
            }
            const MeshPtr m = mesh.as<MeshPtr>(MeshNode::kOutMesh);
            if (!m)
                throw CommandError("bendBones: " + mesh.path() + " has no mesh to bind");
            const glm::dmat4 toWorld = worldOf(mesh);
            b.meshWorld = toWorld;
            std::vector<glm::dvec3> points;
            for (const glm::dvec3& p : m->positions())
                points.push_back(glm::dvec3(toWorld * glm::dvec4(p, 1.0)));
            b.binding = bb::bindPoints(rest, points);

            const std::vector<NodeId> made = deformers::addDeformer(g, kType, {mesh.id()}, a.has("name") ? a.get<std::string>("name") : "BendBones");
            if (made.empty())
                throw CommandError("bendBones: could not add the deformer");
            Node& d = *g.node(made.front());
            const int matrix = d.type().attrIndex("matrix");
            for (size_t i = 0; i < joints.size(); ++i)
                g.connect({joints[i], Transform::kWorldMatrix, -1}, {d.id(), matrix, -1, int(i)});
            g.setValue({d.id(), d.type().attrIndex("volume"), -1}, a.get<double>("volume", 1.0));
            g.setValue({d.id(), d.type().attrIndex("evenBend"), -1}, a.get<double>("evenBend", 1.0));
            g.setValue({d.id(), d.type().attrIndex("binding"), -1}, boundJson(b).dump());
            const int restMatrix = d.type().attrIndex("restMatrix");
            for (size_t i = 0; i < b.rest.size(); ++i)
                g.setValue({d.id(), restMatrix, -1, int(i)}, b.rest[i]);
            g.setValue({d.id(), d.type().attrIndex("bindMeshMatrix"), -1}, b.meshWorld);
            return d.path();
        },
    });
    ops::registerCommand({
        .name = "bendBoneCV",
        .doc = "Add a bone CV to a bend bones bone: an extra point its curve passes through, at a fraction along the bone "
               "from joint to its child. Move or key it to reshape the bone without rebinding. Returns the CV.",
        .positionalDoc = "deformer",
        .flags = {
            {"joint", "j", T::String, nullptr, "The bone's first joint (it runs to the next joint of its chain)."},
            {"at", "at", T::Float, 0.5, "How far along the bone, 0 to 1.", {}, {}, 0.0, 1.0},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<std::string> objs = a.objects();
            Node* d = objs.empty() ? nullptr : g.find(objs.front());
            if (!d || d->type().name != kType)
                throw CommandError("bendBoneCV: give a bendBones deformer");
            if (!a.has("joint"))
                throw CommandError("bendBoneCV: give the bone's first joint");
            const NodeId joint = jointArg(g, a.flags["joint"], "bendBoneCV");
            Bound b = boundOf(json::parse(d->as<std::string>(d->type().attrIndex("binding")), nullptr, false));
            for (const auto& [i, v] : d->elements(d->type().attrIndex("restMatrix")))
                b.rest.push_back(std::get<glm::dmat4>(v));
            // The joint's place in the bind: its index, and the next joint of its chain.
            std::vector<NodeId> joints;
            for (const PlugRef& e : g.elements({d->id(), d->type().attrIndex("matrix"), -1}))
                if (const auto src = g.source(e))
                    joints.push_back(src->node);
            const auto it = std::find(joints.begin(), joints.end(), joint);
            if (it == joints.end())
                throw CommandError("bendBoneCV: " + g.node(joint)->path() + " is not in the deformer's chains");
            const int index = int(it - joints.begin());
            int start = 0;
            bool last = false;
            for (int n : b.sizes)
            {
                if (index < start + n)
                {
                    last = index == start + n - 1;
                    break;
                }
                start += n;
            }
            if (last)
                throw CommandError("bendBoneCV: " + g.node(joint)->path() + " ends its chain, so it has no bone after it");
            g.evaluate();
            const double at = std::clamp(a.get<double>("at", 0.5), 0.0, 1.0);
            const glm::dvec3 restPoint = glm::mix(glm::dvec3(b.rest[size_t(index)][3]), glm::dvec3(b.rest[size_t(index) + 1][3]), at);
            const glm::dvec3 nowPoint = glm::mix(glm::dvec3(worldOf(*g.node(joint))[3]), glm::dvec3(worldOf(*g.node(joints[size_t(index) + 1]))[3]), at);
            Node* cv = g.createNode(*types::find(kCvType), g.node(joint)->name() + "_cv");
            g.setParent(cv->id(), joint, false);
            const glm::dmat4 parentInv = glm::inverse(worldOf(*g.node(joint)));
            g.setValue({cv->id(), Transform::kTranslate, -1}, glm::dvec3(parentInv * glm::dvec4(nowPoint, 1.0)));
            b.cvBone.push_back(index);
            b.cvRest.push_back(glm::dvec3(glm::inverse(b.rest[size_t(index)]) * glm::dvec4(restPoint, 1.0)));
            const int cvMatrix = d->type().attrIndex("cvMatrix");
            g.connect({cv->id(), Transform::kWorldMatrix, -1}, {d->id(), cvMatrix, -1, int(b.cvBone.size() - 1)});
            g.setValue({d->id(), d->type().attrIndex("binding"), -1}, boundJson(b).dump());
            return cv->path();
        },
    });
}
} // namespace lila::scene::bendbones
