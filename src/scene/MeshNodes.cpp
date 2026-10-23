#include "scene/MeshNodes.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Mesh.h"
#include "geo/ObjIO.h"
#include "geo/Primitives.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"

namespace lila::scene
{
namespace
{
AttrDef floatAttr(std::string name, double def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Float, .defaultValue = def, .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc)};
}

AttrDef intAttr(std::string name, int64_t def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Int, .defaultValue = def, .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc)};
}

AttrDef boolAttr(std::string name, bool def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Bool, .defaultValue = def, .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc)};
}

double f(const Node& n, const char* attr) { return n.as<double>(n.type().attrIndex(attr)); }
int i(const Node& n, const char* attr, int lo = 1) { return std::max(lo, int(n.as<int64_t>(n.type().attrIndex(attr)))); }
bool b(const Node& n, const char* attr) { return n.as<bool>(n.type().attrIndex(attr)); }

struct Primitive
{
    const char* type;    // node type, also the command name
    const char* display; // default object name
    std::vector<AttrDef> attrs;
    GeneratorNode::Build build;
};

std::vector<Primitive> primitives()
{
    return {
        {"polyCube", "Cube",
         {floatAttr("width", 100, "Size along X, cm."), floatAttr("height", 100, "Size along the up axis, cm."),
          floatAttr("depth", 100, "Size along Z, cm."), intAttr("subdivisionsX", 1, "Segments across."),
          intAttr("subdivisionsY", 1, "Segments up."), intAttr("subdivisionsZ", 1, "Segments deep."),
          boolAttr("baseAtOrigin", false, "Sit on the ground instead of centring.")},
         [](const Node& n) {
             return geo::makeCube({f(n, "width"), f(n, "height"), f(n, "depth"), i(n, "subdivisionsX"), i(n, "subdivisionsY"),
                                   i(n, "subdivisionsZ"), b(n, "baseAtOrigin")});
         }},
        {"polySphere", "Sphere",
         {floatAttr("radius", 50, "cm."), intAttr("subdivisionsAxis", 20, "Segments around."),
          intAttr("subdivisionsHeight", 20, "Segments pole to pole."), boolAttr("baseAtOrigin", false, "Sit on the ground.")},
         [](const Node& n) {
             return geo::makeSphere({f(n, "radius"), i(n, "subdivisionsAxis", 3), i(n, "subdivisionsHeight", 2), b(n, "baseAtOrigin")});
         }},
        {"polyCylinder", "Cylinder",
         {floatAttr("radius", 50, "cm."), floatAttr("height", 100, "cm."), intAttr("subdivisionsAxis", 20, "Segments around."),
          intAttr("subdivisionsHeight", 1, "Segments up."), intAttr("subdivisionsCaps", 0, "Rings on each cap; 0 is one n-gon."),
          boolAttr("caps", true, "Close the ends."), boolAttr("baseAtOrigin", false, "Sit on the ground.")},
         [](const Node& n) {
             return geo::makeCylinder({f(n, "radius"), f(n, "height"), i(n, "subdivisionsAxis", 3), i(n, "subdivisionsHeight"),
                                       i(n, "subdivisionsCaps", 0), b(n, "caps"), b(n, "baseAtOrigin")});
         }},
        {"polyCapsule", "Capsule",
         {floatAttr("radius", 50, "cm."), floatAttr("height", 200, "End to end, caps included, cm."),
          intAttr("subdivisionsAxis", 20, "Segments around."), intAttr("subdivisionsHeight", 1, "Segments along the straight part."),
          intAttr("subdivisionsCaps", 6, "Rings on each rounded end."), boolAttr("baseAtOrigin", false, "Sit on the ground.")},
         [](const Node& n) {
             return geo::makeCapsule({f(n, "radius"), f(n, "height"), i(n, "subdivisionsAxis", 3), i(n, "subdivisionsHeight"),
                                      i(n, "subdivisionsCaps"), b(n, "baseAtOrigin")});
         }},
        {"polyCone", "Cone",
         {floatAttr("radius", 50, "cm."), floatAttr("height", 100, "cm."), intAttr("subdivisionsAxis", 20, "Segments around."),
          intAttr("subdivisionsHeight", 1, "Segments up."), intAttr("subdivisionsCap", 0, "Rings on the base; 0 is one n-gon."),
          boolAttr("baseAtOrigin", false, "Sit on the ground.")},
         [](const Node& n) {
             return geo::makeCone({f(n, "radius"), f(n, "height"), i(n, "subdivisionsAxis", 3), i(n, "subdivisionsHeight"),
                                   i(n, "subdivisionsCap", 0), b(n, "baseAtOrigin")});
         }},
        {"polyPlane", "Plane",
         {floatAttr("width", 100, "Size along X, cm."), floatAttr("depth", 100, "Size along Z, cm."),
          intAttr("subdivisionsX", 1, "Segments across."), intAttr("subdivisionsZ", 1, "Segments deep.")},
         [](const Node& n) { return geo::makePlane({f(n, "width"), f(n, "depth"), i(n, "subdivisionsX"), i(n, "subdivisionsZ")}); }},
        {"polyTorus", "Torus",
         {floatAttr("radius", 50, "Ring radius, cm."), floatAttr("sectionRadius", 20, "Tube radius, cm."),
          intAttr("subdivisionsAxis", 20, "Segments around the ring."), intAttr("subdivisionsHeight", 20, "Segments around the tube.")},
         [](const Node& n) {
             return geo::makeTorus({f(n, "radius"), f(n, "sectionRadius"), i(n, "subdivisionsAxis", 3), i(n, "subdivisionsHeight", 3)});
         }},
        {"polyDisc", "Disc",
         {floatAttr("radius", 50, "cm."), intAttr("subdivisionsAxis", 20, "Segments around."),
          intAttr("subdivisionsRings", 1, "Rings; 0 is one n-gon.")},
         [](const Node& n) { return geo::makeDisc({f(n, "radius"), i(n, "subdivisionsAxis", 3), i(n, "subdivisionsRings", 0)}); }},
        {"polyPipe", "Pipe",
         {floatAttr("radius", 50, "Outer radius, cm."), floatAttr("thickness", 10, "Wall thickness, cm."), floatAttr("height", 100, "cm."),
          intAttr("subdivisionsAxis", 20, "Segments around."), intAttr("subdivisionsHeight", 1, "Segments up."),
          boolAttr("baseAtOrigin", false, "Sit on the ground.")},
         [](const Node& n) {
             return geo::makePipe({f(n, "radius"), f(n, "thickness"), f(n, "height"), i(n, "subdivisionsAxis", 3),
                                   i(n, "subdivisionsHeight"), b(n, "baseAtOrigin")});
         }},
        {"polyQuadSphere", "QuadSphere",
         {floatAttr("radius", 50, "cm."), intAttr("subdivisions", 8, "Segments per cube face edge.")},
         [](const Node& n) { return geo::makeQuadSphere({f(n, "radius"), i(n, "subdivisions")}); }},
        {"polyIcoSphere", "IcoSphere",
         {floatAttr("radius", 50, "cm."), intAttr("subdivisions", 3, "1 is an icosahedron; each level splits every triangle in four."),
          boolAttr("baseAtOrigin", false, "Sit on the ground.")},
         [](const Node& n) { return geo::makeIcoSphere({f(n, "radius"), i(n, "subdivisions", 1), b(n, "baseAtOrigin")}); }},
    };
}

Graph& graph() { return Graph::current(); }

Node& nodeNamed(const std::string& name)
{
    Node* n = graph().find(name);
    if (!n)
        throw CommandError("no node named '" + name + "'");
    return *n;
}

json vecToJson(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }
} // namespace

void MeshNode::compute(const EvalContext& ctx)
{
    Transform::compute(ctx);
    setOutput(kOutMesh, as<MeshPtr>(kInMesh));
}

NodeType MeshNode::makeType(const NodeType& base)
{
    NodeType t;
    t.name = "mesh";
    t.icon = "cube";
    t.base = &base;
    t.dag = true;
    t.ownAttrs = {
        {.name = "inMesh", .shortName = "i", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Base mesh, or the output of a generator."},
        {.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "Final mesh, as drawn and exported."},
        {.name = "symmetry", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
         .enumNames = {"off", "x", "y", "z"}, .doc = "Mirror plane (object space) every component edit is applied across."},
        {.name = "smoothPreview", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable,
         .doc = "Display-only Catmull-Clark levels (the 3 key); exports stay unsmoothed."},
        {.name = "retopoDrawReference", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden,
         .doc = "Retopo Draw's reference surface: connected from its outMesh when this mesh is being drawn over another.",
         .fileAlias = "quadDrawReference"},
        {.name = "retopoDrawDots", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Retopo Draw's unconnected dots, a JSON list of object-space [x, y, z].", .fileAlias = "quadDrawDots"},
    };
    t.create = [] { return std::make_unique<MeshNode>(); };
    return t;
}

void GeneratorNode::compute(const EvalContext&)
{
    LILA_ZONE();

    try
    {
        setOutput(outAttr, MeshPtr(std::make_shared<geo::Mesh>(build(*this))));
    }
    catch (const std::exception& e)
    {
        log::warn("{}: {}", name(), e.what());
        setOutput(outAttr, MeshPtr());
    }
}

SymmetryState symmetryState(const Graph& g, const std::vector<NodeId>& nodes)
{
    SymmetryState s;
    bool mixed = false;
    for (NodeId id : nodes)
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        ++s.meshes;
        int64_t v = n->as<int64_t>(MeshNode::kSymmetry);
        if (v < 1 || v > 3)
            continue;
        mixed = mixed || (s.on > 0 && s.axis != int(v - 1));
        s.axis = int(v - 1);
        ++s.on;
    }
    if (mixed)
        s.axis = -1;
    return s;
}

void registerMeshTypes()
{
    if (types::find("mesh"))
        return;
    types::registerType(MeshNode::makeType(*types::find("transform")));
    for (Primitive& p : primitives())
    {
        NodeType t;
        t.name = p.type;
        t.icon = "cube-plus";
        t.doc = std::string("Builds the ") + p.display + " primitive from these settings. Editing its components bakes it into a stored base mesh.";
        t.ownAttrs = p.attrs;
        t.ownAttrs.push_back({.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(),
                              .flags = AttrOutput | AttrHidden, .doc = "Generated mesh."});
        int out = int(t.ownAttrs.size()) - 1;
        GeneratorNode::Build build = p.build;
        t.create = [build, out] {
            auto n = std::make_unique<GeneratorNode>();
            n->build = build;
            n->outAttr = out;
            return n;
        };
        types::registerType(std::move(t));
    }
}

json meshStats(Node& n); // ComponentQuery.cpp

void registerMeshCommands()
{
    using T = ArgType;

    for (const Primitive& p : primitives())
    {
        std::vector<FlagDef> flags = {
            {"name", "n", T::String, nullptr, "Name of the mesh; made unique."},
            {"position", "", T::Vec3, nullptr, "World position."},
            {"generator", "", T::Bool, true, "Keep the generator node so its parameters stay editable."},
        };
        for (const AttrDef& a : p.attrs)
            flags.push_back({a.name, "", a.type == PlugType::Bool ? T::Bool : a.type == PlugType::Int ? T::Int : T::Float, nullptr, a.doc});
        std::string type = p.type, display = p.display;
        ops::registerCommand({
            .name = type,
            .doc = std::string("Create a ") + p.display + " mesh. Returns [mesh, generator] (generator is None with generator=False).",
            .flags = flags,
            .run = [type, display](const Args& a) -> json {
                Graph& g = graph();
                const NodeType* genType = types::find(type);
                Node* gen = g.createNode(*genType, {}, kNoNode, true);
                for (auto& [flag, value] : a.flags.items())
                {
                    int idx = genType->attrIndex(flag);
                    if (idx >= 0 && !value.is_null())
                        g.setValue({gen->id(), idx, -1}, fromJson(genType->attrs[idx], value));
                }
                Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", display), kNoNode, true);
                g.connect({gen->id(), static_cast<GeneratorNode*>(gen)->outAttr, -1}, {mesh->id(), MeshNode::kInMesh, -1});
                if (a.has("position"))
                    g.setValue({mesh->id(), Transform::kTranslate, -1},
                               glm::dvec3(a.flags["position"][0], a.flags["position"][1], a.flags["position"][2]));
                json genName = gen->path();
                if (!a.get<bool>("generator", true))
                {
                    g.evaluate();
                    MeshPtr baked = mesh->as<MeshPtr>(MeshNode::kOutMesh);
                    g.disconnect({mesh->id(), MeshNode::kInMesh, -1});
                    g.setValue({mesh->id(), MeshNode::kInMesh, -1}, baked);
                    g.deleteNode(gen->id());
                    genName = nullptr;
                }
                g.select({mesh->id()});
                return json::array({mesh->path(), genName});
            },
        });
    }

    ops::registerCommand({
        .name = "smoothPreview",
        .doc = "Display meshes subdivided without changing them (the 1/2/3 keys). level 0 is off.",
        .positionalDoc = "objects",
        .flags = {{"level", "", T::Int, 2, "Subdivision levels to display (0 to 3)."}},
        .run = [](const Args& a) -> json {
            int64_t level = std::clamp<int64_t>(a.get<int>("level", 2), 0, 3);
            for (const std::string& name : a.objects())
                if (Node& n = nodeNamed(name); n.type().isA("mesh"))
                    graph().setValue({n.id(), MeshNode::kSmoothPreview, -1}, level);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "meshInfo",
        .doc = "Counts and health of a mesh: points, edges, faces, triangles, tri/quad/ngon faces, open border edges and loops, "
               "non-manifold edges, shells, closed, object and world bounds, symmetry, smooth preview, modifiers, material slots "
               "(and the cage's counts under modifiers).",
        .positionalDoc = "object",
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("meshInfo: no object");
            Node& n = nodeNamed(objs[0]);
            if (!n.type().isA("mesh"))
                throw CommandError("meshInfo: " + objs[0] + " is not a mesh");
            return meshStats(n);
        },
    });

    ops::registerCommand({
        .name = "meshData",
        .doc = "A mesh's points and faces as plain lists: {points: [[x,y,z]...], faces: [[i,j,k,...]...]}. "
               "base=True reads the editable base mesh (under modifiers) instead of the final result.",
        .positionalDoc = "mesh",
        .flags = {{"base", "", T::Bool, false, "The base mesh, not the modified result."},
                  {"worldSpace", "", T::Bool, false, "Points in world space."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("meshData: no mesh");
            Node& n = nodeNamed(objs.back());
            if (!n.type().isA("mesh"))
                throw CommandError("meshData: " + objs.back() + " is not a mesh");
            graph().evaluate();
            MeshPtr m = a.get<bool>("base") ? modifiers::cage(graph(), n.id()) : n.as<MeshPtr>(MeshNode::kOutMesh);
            json points = json::array(), faces = json::array();
            if (!m)
                return {{"points", points}, {"faces", faces}};
            glm::dmat4 w = a.get<bool>("worldSpace") ? n.as<glm::dmat4>(Transform::kWorldMatrix) : glm::dmat4(1.0);
            std::vector<geo::Index> remap(m->pointCapacity(), geo::kInvalid);
            for (geo::Index p = 0; p < m->pointCapacity(); ++p)
                if (m->pointValid(p))
                {
                    remap[p] = geo::Index(points.size());
                    glm::dvec3 q = glm::dvec3(w * glm::dvec4(m->position(p), 1.0));
                    points.push_back({q.x, q.y, q.z});
                }
            for (geo::Index f = 0; f < m->faceCapacity(); ++f)
                if (m->faceValid(f))
                {
                    json face = json::array();
                    m->forEachFaceCorner(f, [&](geo::Index c) { face.push_back(remap[m->cornerPoint(c)]); });
                    faces.push_back(std::move(face));
                }
            return {{"points", std::move(points)}, {"faces", std::move(faces)}};
        },
    });

    ops::registerCommand({
        .name = "createMesh",
        .doc = "Make a mesh from points and faces (lists of point indices, any size). Returns its name.",
        .flags = {{"points", "", T::List, nullptr, "[[x, y, z], ...] in cm."},
                  {"faces", "", T::List, nullptr, "[[i, j, k, ...], ...]."},
                  {"name", "n", T::String, nullptr, "Name; made unique."}},
        .run = [](const Args& a) -> json {
            if (!a.has("points") || !a.has("faces"))
                throw CommandError("createMesh: give points= and faces=");
            std::vector<glm::dvec3> pts;
            for (const json& p : a.flags["points"])
                pts.emplace_back(p.at(0).get<double>(), p.at(1).get<double>(), p.at(2).get<double>());
            std::vector<geo::Index> sizes, idx;
            for (const json& f : a.flags["faces"])
            {
                sizes.push_back(geo::Index(f.size()));
                for (const json& i : f)
                {
                    geo::Index v = i.get<geo::Index>();
                    if (v >= pts.size())
                        throw CommandError("createMesh: face index " + std::to_string(v) + " is out of range");
                    idx.push_back(v);
                }
            }
            Node* n = graph().createNode(*types::find("mesh"), a.get<std::string>("name", "Mesh"), kNoNode, true);
            graph().setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(geo::Mesh::fromPolygons(pts, sizes, idx))));
            graph().select({n->id()});
            return n->path();
        },
    });

    ops::registerCommand({
        .name = "importObj",
        .doc = "Import an OBJ file; one mesh per object. Returns the new mesh names.",
        .positionalDoc = "path",
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("importObj: path required");
            std::string path = a.positional[0].get<std::string>();
            progress::Scope scope("Importing " + path + "...", 4.0);
            geo::ObjReadResult r;
            {
                progress::Scope read("Reading " + path, 0.0, progress::None, 3.0);
                progress::runOnWorker([&] { r = geo::readObjFile(path); });
            }
            if (!r.ok())
                throw CommandError("importObj: " + r.error);
            Graph& g = graph();
            Graph::Batch batch(g);
            json names = json::array();
            std::vector<NodeId> ids;
            for (geo::ObjObject& o : r.objects)
            {
                std::string wanted = Graph::validName(o.name) ? o.name : "Mesh";
                Node* n = g.createNode(*types::find("mesh"), wanted, kNoNode, true);
                g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(o.mesh))));
                names.push_back(n->path());
                ids.push_back(n->id());
            }
            g.select(ids);
            return names;
        },
    });

    ops::registerCommand({
        .name = "exportObj",
        .doc = "Export meshes to an OBJ file in world space. Defaults to the selection.",
        .positionalDoc = "path objects...",
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("exportObj: path required");
            std::string path = a.positional[0].get<std::string>();
            std::vector<std::string> names;
            for (size_t k = 1; k < a.positional.size(); ++k)
                names.push_back(a.positional[k].get<std::string>());
            if (names.empty())
                for (NodeId id : graph().selection())
                    names.push_back(graph().node(id)->path());
            Graph& g = graph();
            g.evaluate();
            std::vector<geo::ObjObject> objects;
            for (const std::string& name : names)
            {
                Node& n = nodeNamed(name);
                if (!n.type().isA("mesh"))
                    continue;
                MeshPtr m = modifiers::exportMesh(g, n.id());
                if (!m)
                    continue;
                geo::ObjObject o;
                o.name = name;
                o.mesh = *m;
                glm::dmat4 world = n.as<glm::dmat4>(Transform::kWorldMatrix);
                for (geo::Index p = 0; p < o.mesh.pointCapacity(); ++p)
                    if (o.mesh.pointValid(p))
                        o.mesh.setPosition(p, glm::dvec3(world * glm::dvec4(o.mesh.position(p), 1.0)));
                objects.push_back(std::move(o));
            }
            if (objects.empty())
                throw CommandError("exportObj: no meshes to export");
            progress::Scope scope("Exporting " + path + "...");
            bool written = false;
            progress::runOnWorker([&] {
                std::string text = geo::writeObj(objects);
                if (FILE* fp = std::fopen(path.c_str(), "wb"))
                {
                    std::fwrite(text.data(), 1, text.size(), fp);
                    std::fclose(fp);
                    written = true;
                }
            });
            if (!written)
                throw CommandError("exportObj: can't write " + path);
            return path;
        },
    });
}
} // namespace lila::scene
