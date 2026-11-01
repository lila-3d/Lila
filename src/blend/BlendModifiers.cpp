#include "blend/BlendInternal.h"

#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/MeshNodes.h"
#include "vcs/Process.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

namespace lila::blend
{
namespace
{
/** Modifiers Lila rebuilds live, Smooth by Angle (read into the hard edges) and the armature (the skin). */
bool handledNatively(const View& mod, int type)
{
    switch (type)
    {
    case md::kMirror:
    case md::kSubsurf:
    case md::kBevel:
    case md::kArray:
    case md::kTriangulate:
    case md::kSolidify:
    case md::kDecimate:
    case md::kWeightedNormal:
    case md::kArmature: return true;
    case md::kNodes:
    {
        const View group = mod.deref("node_group");
        return group && group.idName().starts_with("Smooth by Angle");
    }
    default: return false;
    }
}

/** The Lila modifier kind and params for a Blender modifier, empty kind when there is none. */
std::vector<std::pair<std::string, json>> lilaModifiers(const View& mod, int type, const Axes& axes, const geo::Mesh* base, std::vector<std::string>& notes)
{
    std::vector<std::pair<std::string, json>> out;
    switch (type)
    {
    case md::kMirror:
    {
        const int flag = mod.i("flag");
        // MOD_MIR_AXIS_X/Y/Z are bits 3, 4 and 5; NO_MERGE bit 7; bisect bits 8 to 10.
        for (int a = 0; a < 3; ++a)
            if (flag & (1 << (3 + a)))
                out.push_back({"mirror",
                               {{"axis", axes.axis[a]},
                                {"direction", (flag & (1 << (8 + a))) ? (((flag & (1 << (11 + a))) != 0) != (axes.sign[a] < 0.0) ? 1 : 0) : 2},
                                {"weld", !(flag & (1 << 7))},
                                {"tolerance", mod.num("tolerance", 0, 0.001) * axes.cm}}});
        if (mod.ptr("mirror_ob"))
            notes.push_back("mirror object (mirrored about the mesh's own origin)");
        break;
    }
    case md::kSubsurf:
        if (mod.i("subdivType") == 1)
            notes.push_back("simple subdivision (Catmull-Clark used)");
        out.push_back({"subdivision", {{"levels", std::clamp(mod.i("levels"), 0, 4)}}});
        break;
    case md::kBevel:
    {
        if (mod.i("affect_type") == 0)
        {
            notes.push_back("vertex bevel");
            break;
        }
        const int lim = mod.i("lim_flags");
        double angle = 0.0;
        if (lim & (1 << 3))
            angle = glm::degrees(mod.num("bevel_angle", 0, 0.5236));
        else if (lim & (1 << 4))
            notes.push_back("bevel limited by weight (bevels every edge)");
        else if (lim & (1 << 5))
            notes.push_back("bevel limited by vertex group (bevels every edge)");
        out.push_back({"bevel", {{"offset", mod.num("value", 0, 0.1) * axes.cm}, {"segments", std::clamp(mod.i("res"), 1, 32)}, {"profile", mod.num("profile", 0, 0.5)}, {"angle", angle}}});
        break;
    }
    case md::kArray:
    {
        if (mod.i("fit_type") != 0)
            notes.push_back("array fit to length or curve (its count is used)");
        if (mod.ptr("start_cap") || mod.ptr("end_cap") || mod.ptr("offset_ob"))
            notes.push_back("array caps or object offset");
        const int offsetType = mod.i("offset_type");
        glm::dvec3 offset(0.0);
        if (offsetType & 1)
            offset += axes.direction(mod.vec3("offset")) * axes.cm;
        if (offsetType & 2)
        {
            glm::dvec3 lo(1e300), hi(-1e300);
            if (base)
                for (geo::Index p = 0; p < base->pointCapacity(); ++p)
                    if (base->pointValid(p))
                    {
                        lo = glm::min(lo, base->position(p));
                        hi = glm::max(hi, base->position(p));
                    }
            const glm::dvec3 size = base && lo.x <= hi.x ? hi - lo : glm::dvec3(0.0);
            const glm::dvec3 rel = mod.vec3("scale");
            for (int a = 0; a < 3; ++a)
                offset[axes.axis[a]] += axes.sign[a] * rel[a] * size[axes.axis[a]];
        }
        const int flags = mod.i("flags");
        out.push_back({"array",
                       {{"count", std::clamp(mod.i("count"), 1, 1000)},
                        {"offset", {offset.x, offset.y, offset.z}},
                        {"weld", (flags & 1) != 0},
                        {"mergeEnds", (flags & 2) != 0},
                        {"tolerance", mod.num("merge_dist", 0, 0.01) * axes.cm}}});
        break;
    }
    case md::kTriangulate: out.push_back({"triangulate", json::object()}); break;
    case md::kSolidify:
        if (mod.num("offset_fac", 0, -1.0) > -0.99)
            notes.push_back("solidify offset other than inward");
        out.push_back({"solidify", {{"thickness", mod.num("offset", 0, 0.01) * axes.cm}}});
        break;
    case md::kDecimate:
        if (mod.i("mode") != 0)
        {
            notes.push_back("un-subdivide or planar decimate");
            break;
        }
        out.push_back({"decimate", {{"ratio", std::clamp(mod.num("percent", 0, 1.0), 0.0, 1.0)}, {"keepBorders", false}, {"keepHardEdges", false}}});
        break;
    case md::kWeightedNormal: out.push_back({"weightedNormals", {{"keepHardEdges", true}}}); break;
    default: break;
    }
    return out;
}

fs::path findBlender(const std::string& configured)
{
    std::error_code ec;
    if (!configured.empty())
    {
        fs::path p(std::u8string(configured.begin(), configured.end()));
        return fs::exists(p, ec) ? p : fs::path();
    }
#ifdef _WIN32
    // The newest installed version.
    fs::path best;
    const fs::path root = "C:/Program Files/Blender Foundation";
    for (const auto& e : fs::directory_iterator(root, ec))
        if (fs::exists(e.path() / "blender.exe", ec) && (best.empty() || e.path().filename() > best.parent_path().filename()))
            best = e.path() / "blender.exe";
    if (!best.empty())
        return best;
#elif defined(__APPLE__)
    if (fs::exists("/Applications/Blender.app/Contents/MacOS/Blender", ec))
        return "/Applications/Blender.app/Contents/MacOS/Blender";
#endif
    return vcs::findExecutable("blender");
}

/** Writes each listed object's evaluated mesh in its object space: a small binary file per object. */
constexpr const char* kEvaluateScript = R"PY(
import bpy, struct, sys, os
args = sys.argv[sys.argv.index("--") + 1:]
out_dir, names = args[0], args[1:]
# Objects only seen through collection instances are outside the view layer, so nothing evaluates them: link them in.
layer = bpy.context.view_layer.objects
extra = bpy.data.collections.new("lila_evaluate")
bpy.context.scene.collection.children.link(extra)
for name in names:
    ob = bpy.data.objects.get(name)
    if ob is not None and layer.get(ob.name) is None:
        extra.objects.link(ob)
deps = bpy.context.evaluated_depsgraph_get()

def instanced(ob):
    """The geometry nodes' instances of ob, joined into one temporary mesh in ob's space (to_mesh leaves them out)."""
    import bmesh
    inv = ob.matrix_world.inverted()
    bm = bmesh.new()
    for inst in deps.object_instances:
        if not inst.is_instance or inst.parent is None or inst.parent.original != ob or inst.object.type != "MESH":
            continue
        part = bpy.data.meshes.new_from_object(inst.object, preserve_all_data_layers=True, depsgraph=deps)
        part.transform(inv @ inst.matrix_world.copy())
        bm.from_mesh(part)
        bpy.data.meshes.remove(part)
    if not bm.verts:
        bm.free()
        return None
    out = bpy.data.meshes.new("lila_instances")
    bm.to_mesh(out)
    bm.free()
    return out

for i, name in enumerate(names):
    ob = bpy.data.objects.get(name)
    if ob is None:
        continue
    e = ob.evaluated_get(deps)
    try:
        m = e.to_mesh()
    except RuntimeError:
        m = None
    if m is None or len(m.polygons) == 0:
        m = instanced(ob) or m
    if m is None:
        continue
    nv, nf, nc = len(m.vertices), len(m.polygons), len(m.loops)
    pos = [0.0] * (nv * 3); m.vertices.foreach_get("co", pos)
    sizes = [0] * nf; m.polygons.foreach_get("loop_total", sizes)
    corners = [0] * nc; m.loops.foreach_get("vertex_index", corners)
    mats = [0] * nf; m.polygons.foreach_get("material_index", mats)
    normals = [0.0] * (nc * 3)
    try:
        m.corner_normals.foreach_get("vector", normals)
    except Exception:
        pass
    layers = list(m.uv_layers)
    default = next((l for l in layers if getattr(l, "active_render", False)), None)
    if default is not None:
        layers.remove(default)
        layers.insert(0, default)
    uvs = []
    for layer in layers:
        uv = [0.0] * (nc * 2); layer.data.foreach_get("uv", uv)
        uvs.append((layer.name, uv))
    with open(os.path.join(out_dir, "%d.bin" % i), "wb") as f:
        f.write(struct.pack("<4i", nv, nf, nc, len(uvs)))
        f.write(struct.pack("<%df" % len(pos), *pos))
        f.write(struct.pack("<%di" % nf, *sizes))
        f.write(struct.pack("<%di" % nc, *corners))
        f.write(struct.pack("<%di" % nf, *mats))
        f.write(struct.pack("<%df" % len(normals), *normals))
        for name, uv in uvs:
            b = name.encode("utf-8")
            f.write(struct.pack("<i", len(b)) + b)
            f.write(struct.pack("<%df" % len(uv), *uv))
    if m.name == "lila_instances":
        bpy.data.meshes.remove(m)
    else:
        e.to_mesh_clear()
)PY";

template <class T>
bool readArray(std::ifstream& in, std::vector<T>& v, size_t n)
{
    v.resize(n);
    in.read(reinterpret_cast<char*>(v.data()), std::streamsize(n * sizeof(T)));
    return bool(in);
}
} // namespace

void Importer::buildModifiers()
{
    LILA_ZONE();

    for (const ModifierJob& job : modifierJobs)
    {
        const View object = job.object;
        std::vector<View> mods = object.list("modifiers");
        bool unsupported = false;
        std::string dropped;
        for (const View& mod : mods)
        {
            const View head = mod.has("modifier") ? mod.sub("modifier") : mod;
            if (!(head.i("mode") & 1))
                continue;
            const int type = head.i("type");
            if (!handledNatively(mod, type))
            {
                unsupported = true;
                const View group = type == md::kNodes ? mod.deref("node_group") : View();
                dropped += std::format("{}'{}' ({}{})", dropped.empty() ? "" : ", ", head.str("name"), md::name(type), group ? ": " + group.idName() : "");
            }
        }
        const bool evaluate = !armatureOf(object) && !object.deref("data").ptr("key") &&
                              ((opts.evaluate == "unsupported" && unsupported) || (opts.evaluate == "all" && !mods.empty()));
        if (evaluate)
        {
            toEvaluate.push_back({job.mesh, object.idName(), false, {}});
            continue;
        }
        if (!opts.modifiers)
            continue;

        const std::string meshPath = g.node(job.mesh)->path();
        const auto base = g.node(job.mesh) ? g.storedValue({job.mesh, scene::MeshNode::kInMesh, -1}) : scene::PlugValue();
        const geo::Mesh* baseMesh = nullptr;
        if (const auto* mp = std::get_if<scene::MeshPtr>(&base))
            baseMesh = mp->get();
        std::vector<std::string> notes;
        bool stopped = false;
        for (const View& mod : mods)
        {
            const View head = mod.has("modifier") ? mod.sub("modifier") : mod;
            if (!(head.i("mode") & 1))
                continue;
            const int type = head.i("type");
            if (!handledNatively(mod, type))
            {
                stopped = true;
                break;
            }
            for (const auto& [kind, params] : lilaModifiers(mod, type, axes, baseMesh, notes))
            {
                try
                {
                    const json made = ops::execute("addModifier", {{"args", {meshPath}}, {"kind", kind}, {"params", params}});
                    if (made.is_string())
                        r.modifiers.push_back(made.get<std::string>());
                }
                catch (const std::exception& e)
                {
                    r.warnings.push_back(std::format("'{}': {} modifier: {}", object.idName(), md::name(type), e.what()));
                }
            }
        }
        if (stopped || !notes.empty())
        {
            std::string message;
            if (stopped)
                message = std::format("Blender modifiers not converted: {}. The mesh shows the stack before the first of them.", dropped);
            for (const std::string& n : notes)
                message += (message.empty() ? "" : " ") + std::format("Approximated: {}.", n);
            issue(job.mesh, message, stopped);
            r.skipped.push_back(std::format("'{}': {}", object.idName(), message));
        }
    }
}

void Importer::evaluateWithBlender()
{
    if (toEvaluate.empty())
        return;
    LILA_ZONE();

    const fs::path blender = findBlender(opts.blenderPath);
    if (blender.empty())
    {
        r.warnings.push_back("Evaluate with Blender: no Blender found; set blenderPath");
        return;
    }
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / std::format("lila_blend_eval_{}", reinterpret_cast<uintptr_t>(this));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path script = dir / "evaluate.py";
    {
        std::ofstream f(script);
        f << kEvaluateScript;
    }
    vcs::ProcessSpec spec;
    spec.argv = {utf8(blender), "-b", "--factory-startup", utf8(file.path()), "--python", utf8(script), "--", utf8(dir)};
    for (const Evaluate& e : toEvaluate)
        spec.argv.push_back(e.name);
    spec.timeoutMs = 30 * 60 * 1000;
    vcs::ProcessResult res;
    {
        progress::Scope wait("Evaluating in Blender", 1.0);
        res = vcs::systemRunner()->run(spec);
    }
    if (!res.started)
    {
        r.warnings.push_back(std::format("Evaluate with Blender: {} didn't start", utf8(blender)));
        fs::remove_all(dir, ec);
        return;
    }
    // A crash on the way out still leaves every mesh written before it.
    if (res.exitCode != 0)
        r.warnings.push_back(std::format("Evaluate with Blender: {} exited with code {}; the meshes it wrote are used", utf8(blender), res.exitCode));
    for (size_t i = 0; i < toEvaluate.size(); ++i)
    {
        const std::string& name = toEvaluate[i].name;
        NodeId node = toEvaluate[i].node;
        std::ifstream in(dir / std::format("{}.bin", i), std::ios::binary);
        int32_t head[4] = {};
        if (!in || !in.read(reinterpret_cast<char*>(head), sizeof(head)) || head[0] < 0 || head[1] < 0 || head[2] < 0)
        {
            r.warnings.push_back(std::format("Evaluate with Blender: no result for '{}'", name));
            continue;
        }
        std::vector<float> pos, normals;
        std::vector<int32_t> sizes, corners, mats;
        std::vector<std::vector<float>> uvLayers(size_t(std::clamp(head[3], 0, 8)));
        std::vector<std::string> uvNames(uvLayers.size());
        bool ok = readArray(in, pos, size_t(head[0]) * 3) && readArray(in, sizes, size_t(head[1])) && readArray(in, corners, size_t(head[2])) &&
                  readArray(in, mats, size_t(head[1])) && readArray(in, normals, size_t(head[2]) * 3);
        for (size_t s = 0; s < uvLayers.size() && ok; ++s)
        {
            int32_t len = 0;
            ok = bool(in.read(reinterpret_cast<char*>(&len), 4)) && len >= 0 && len < 4096;
            if (ok)
            {
                uvNames[s].resize(size_t(len));
                ok = bool(in.read(uvNames[s].data(), len)) && readArray(in, uvLayers[s], size_t(head[2]) * 2);
            }
        }
        if (!ok)
        {
            r.warnings.push_back(std::format("Evaluate with Blender: '{}' came back damaged", name));
            continue;
        }
        if (toEvaluate[i].curve)
        {
            // The curve stays; the mesh its modifiers make rides along as a child.
            node = createNode("mesh", g.node(node)->name() + "Mesh", node);
            r.meshes.push_back(node);
        }
        const scene::PlugValue old = g.storedValue({node, scene::MeshNode::kInMesh, -1});
        const auto* oldMesh = std::get_if<scene::MeshPtr>(&old);
        auto m = std::make_shared<geo::Mesh>();
        for (size_t p = 0; p < size_t(head[0]); ++p)
            m->addPoint(axes.point(glm::dvec3(pos[p * 3], pos[p * 3 + 1], pos[p * 3 + 2])));
        if (oldMesh && *oldMesh)
            m->materialNames() = (*oldMesh)->materialNames();
        else if (toEvaluate[i].curve)
            m->materialNames() = toEvaluate[i].slots;
        // Faces first, then the corner values through each Blender corner's Lila corner.
        std::vector<geo::Index> cornerOf(corners.size(), geo::kInvalid);
        std::vector<std::pair<geo::Index, int32_t>> faceMats;
        std::vector<geo::Index> pts;
        size_t at = 0;
        for (size_t f = 0; f < size_t(head[1]); ++f)
        {
            const size_t n = size_t(std::max(0, sizes[f]));
            if (at + n > corners.size())
                break;
            pts.assign(corners.begin() + std::ptrdiff_t(at), corners.begin() + std::ptrdiff_t(at + n));
            const size_t first = at;
            at += n;
            if (std::any_of(pts.begin(), pts.end(), [&](geo::Index p) { return p >= geo::Index(head[0]); }))
                continue;
            const geo::Index face = m->addFace(pts);
            if (face == geo::kInvalid)
                continue;
            geo::Index c = m->findCorner(face, pts.front());
            for (size_t k = first; k < at && c != geo::kInvalid; ++k, c = m->cornerNext(c))
                cornerOf[k] = c;
            faceMats.push_back({face, mats[f]});
        }
        std::span<glm::vec3> nrm = m->cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
        for (size_t k = 0; k < cornerOf.size(); ++k)
            if (cornerOf[k] != geo::kInvalid)
            {
                const glm::dvec3 n = axes.direction(glm::dvec3(normals[k * 3], normals[k * 3 + 1], normals[k * 3 + 2]));
                nrm[cornerOf[k]] = glm::vec3(glm::length(n) > 0.0 ? glm::normalize(n) : n);
            }
        // The UV map the materials read goes first, as on meshes read from the file.
        if (const std::string want = preferredUv(m->materialNames()); !want.empty())
            if (auto at = std::find(uvNames.begin(), uvNames.end(), want); at != uvNames.end() && at != uvNames.begin())
            {
                const size_t k = size_t(at - uvNames.begin());
                std::rotate(uvLayers.begin(), uvLayers.begin() + std::ptrdiff_t(k), uvLayers.begin() + std::ptrdiff_t(k) + 1);
                std::rotate(uvNames.begin(), uvNames.begin() + std::ptrdiff_t(k), uvNames.begin() + std::ptrdiff_t(k) + 1);
            }
        for (size_t s = 0; s < uvLayers.size(); ++s)
        {
            std::span<glm::vec2> uvs = m->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(int(s)));
            for (size_t k = 0; k < cornerOf.size(); ++k)
                if (cornerOf[k] != geo::kInvalid)
                    uvs[cornerOf[k]] = glm::vec2(uvLayers[s][k * 2], uvLayers[s][k * 2 + 1]);
        }
        if (!m->materialNames().empty())
        {
            std::span<int32_t> mat = m->faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
            for (const auto& [face, index] : faceMats)
                mat[face] = std::clamp<int32_t>(index, 0, int32_t(m->materialNames().size()) - 1);
        }
        g.setValue({node, scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::move(m)));
        r.evaluated.push_back(name);
        issue(node, "Evaluated in Blender: its modifiers are baked into the mesh.", false);
    }
    fs::remove_all(dir, ec);
}
} // namespace lila::blend
