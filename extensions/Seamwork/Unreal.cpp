// S5: contacts out of Lila. seamSend (materials, splines with tags, PCG points and splines, blend ids) and seamExport (JSON
// or USD for Godot, Unity and USD pipelines).

#include "Seamwork.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>

#include <cmath>
#include <fstream>
#include <iomanip>

using namespace lila;
using namespace lila::scene;

namespace seamwork
{
using T = ArgType;

namespace
{
json unrealVec(const glm::dvec3& v)
{
    const math::Vector u = math::toUnreal(math::Vector(v));
    auto clean = [](double x) { return std::abs(x) < 1e-9 ? 0.0 : x; };
    return json::array({clean(u.x), clean(u.y), clean(u.z)});
}

double mean(const std::vector<double>& v)
{
    double s = 0.0;
    for (double x : v)
        s += x;
    return v.empty() ? 0.0 : s / double(v.size());
}

double maxOf(const std::vector<double>& v)
{
    double m = 0.0;
    for (double x : v)
        m = std::max(m, x);
    return m;
}

std::string fmt(double v)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(2) << v;
    return s.str();
}

json pointAttributes(const SeamData& d, const SeamPoint& p)
{
    return {{"angle", p.angle},
            {"depth", p.depth},
            {"gap", p.gap},
            {"upFacing", p.upFacing},
            {"exposure", p.exposure},
            {"kind", kindName(p.kind)},
            {"objectA", objectName(d, p.a)},
            {"objectB", objectName(d, p.b)},
            {"layer", p.layer >= 0 && size_t(p.layer) < d.result->layers.size() ? d.result->layers[size_t(p.layer)] : std::string()},
            {"normalA", v3(p.normalA)},
            {"normalB", v3(p.normalB)}};
}

/** A point's frame for PCG: Z along the bisector out of the wedge, X along the join. */
glm::dmat4 pointFrame(const SeamPoint& p, const glm::dvec3& up)
{
    const glm::dvec3 z = bisector(p, up);
    glm::dvec3 x = p.tangent - z * glm::dot(p.tangent, z);
    x = glm::length(x) > 1e-9 ? glm::normalize(x) : glm::normalize(glm::cross(std::abs(z.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0), z));
    const glm::dvec3 y = glm::cross(z, x);
    glm::dmat4 m(glm::dvec4(x, 0.0), glm::dvec4(y, 0.0), glm::dvec4(z, 0.0), glm::dvec4(p.position, 1.0));
    return m;
}

std::vector<link::PcgSet> pcgSets(const SeamData& d)
{
    std::vector<link::PcgSet> sets;
    link::PcgSet pts;
    pts.kind = link::PcgSet::Kind::Points;
    pts.pin = "Points";
    pts.tag = "Contacts";
    for (const SeamPoint& p : d.points)
    {
        link::PcgPoint q;
        q.world = pointFrame(p, d.up);
        q.boundsMin = glm::dvec3(-5.0);
        q.boundsMax = glm::dvec3(5.0);
        q.density = std::clamp(1.0 - p.exposure * 0.5, 0.0, 1.0);
        q.attributes = pointAttributes(d, p);
        pts.points.push_back(std::move(q));
    }
    sets.push_back(std::move(pts));
    for (size_t i = 0; i < d.result->curves.size(); ++i)
    {
        const Curve& c = d.result->curves[i];
        link::PcgSet s;
        s.kind = link::PcgSet::Kind::Spline;
        s.pin = "Splines";
        s.tag = std::string("Contact_") + kindName(c.kind);
        s.path = c.points;
        s.closed = c.closed;
        s.attributes = {{"kind", kindName(c.kind)}, {"objectA", objectName(d, c.a)}, {"objectB", objectName(d, c.b)}, {"near", c.nearContact},
                        {"angle", mean(c.angle)}, {"depth", maxOf(c.depth)}, {"exposure", mean(c.exposure)}, {"length", c.length()}};
        sets.push_back(std::move(s));
    }
    return sets;
}

int64_t intAttr(const Node& n, const char* name)
{
    const int i = n.type().attrIndex(name);
    if (i < 0)
        return 0;
    const int64_t* v = std::get_if<int64_t>(&n.value(i));
    return v ? *v : 0;
}

void setUserAttr(Graph& g, NodeId id, const char* name, PlugType type, const PlugValue& v)
{
    Node* n = g.node(id);
    int i = n->type().attrIndex(name);
    if (i < 0)
        i = g.addAttr(id, {.name = name, .type = type, .defaultValue = v, .flags = AttrInput | AttrStorable | AttrUser | AttrHidden, .doc = "Seamwork."});
    g.setValue({id, i, -1}, v);
}

/** Gives each object a contactBlendId number and adds it to what unrealPlaceInstances sends as custom primitive data. */
json assignBlendIds(Graph& g, const SeamData& d)
{
    json out = json::array();
    for (size_t i = 0; i < d.objects.size(); ++i)
    {
        Node* n = g.find(d.objects[i].name);
        if (!n || d.objects[i].ground || !n->type().isA("mesh"))
            continue;
        setUserAttr(g, n->id(), "contactBlendId", PlugType::Float, double(i + 1));
        int slot = -1;
        if (ops::find("unrealCustomData"))
        {
            json names = ops::execute("unrealCustomData", {{"args", {n->path()}}, {"query", true}});
            if (!names.is_array())
                names = json::array({"random"});
            auto it = std::find(names.begin(), names.end(), json("contactBlendId"));
            if (it == names.end())
            {
                names.push_back("contactBlendId");
                ops::execute("unrealCustomData", {{"args", {n->path()}}, {"data", names}});
                it = std::find(names.begin(), names.end(), json("contactBlendId"));
            }
            slot = int(it - names.begin());
        }
        out.push_back({{"node", n->path()}, {"id", i + 1}, {"slot", slot}});
    }
    return out;
}

void writeUsd(const std::filesystem::path& file, const SeamData& d)
{
    std::ofstream f(file);
    if (!f)
        throw CommandError("seamExport: cannot write " + file.string());
    f << std::setprecision(9);
    f << "#usda 1.0\n(\n    defaultPrim = \"Contacts\"\n    metersPerUnit = 0.01\n    upAxis = \"" << (d.up.z > 0.5 ? "Z" : "Y") << "\"\n)\n\n";
    f << "def Xform \"Contacts\"\n{\n";
    auto vec = [&](const glm::dvec3& v) { f << "(" << v.x << ", " << v.y << ", " << v.z << ")"; };
    auto list = [&](const char* type, const char* name, const std::vector<double>& v, const char* interp) {
        f << "        " << type << "[] primvars:" << name << " = [";
        for (size_t k = 0; k < v.size(); ++k)
            f << (k ? ", " : "") << v[k];
        f << "] (\n            interpolation = \"" << interp << "\"\n        )\n";
    };
    for (size_t i = 0; i < d.result->curves.size(); ++i)
    {
        const Curve& c = d.result->curves[i];
        f << "    def BasisCurves \"curve_" << i << "\"\n    {\n        uniform token type = \"linear\"\n        uniform token wrap = \""
          << (c.closed ? "periodic" : "nonperiodic") << "\"\n";
        f << "        int[] curveVertexCounts = [" << c.points.size() << "]\n        point3f[] points = [";
        for (size_t k = 0; k < c.points.size(); ++k)
        {
            f << (k ? ", " : "");
            vec(c.points[k]);
        }
        f << "]\n";
        list("float", "angle", c.angle, "vertex");
        list("float", "depth", c.depth, "vertex");
        list("float", "gap", c.gap, "vertex");
        list("float", "upFacing", c.upFacing, "vertex");
        list("float", "exposure", c.exposure, "vertex");
        f << "        string primvars:kind = \"" << kindName(c.kind) << "\" (\n            interpolation = \"constant\"\n        )\n";
        f << "        string primvars:objectA = \"" << objectName(d, c.a) << "\" (\n            interpolation = \"constant\"\n        )\n";
        f << "        string primvars:objectB = \"" << objectName(d, c.b) << "\" (\n            interpolation = \"constant\"\n        )\n";
        f << "    }\n";
    }
    f << "    def Points \"points\"\n    {\n        point3f[] points = [";
    for (size_t k = 0; k < d.points.size(); ++k)
    {
        f << (k ? ", " : "");
        vec(d.points[k].position);
    }
    f << "]\n";
    std::vector<double> angle, depth, exposure, kind;
    for (const SeamPoint& p : d.points)
    {
        angle.push_back(p.angle);
        depth.push_back(p.depth);
        exposure.push_back(p.exposure);
        kind.push_back(double(int(p.kind)));
    }
    list("float", "angle", angle, "vertex");
    list("float", "depth", depth, "vertex");
    list("float", "exposure", exposure, "vertex");
    list("int", "kind", kind, "vertex");
    f << "    }\n}\n";
}

json exportJson(const SeamData& d)
{
    json curves = json::array();
    for (const Curve& c : d.result->curves)
    {
        json pts = json::array();
        for (size_t k = 0; k < c.points.size(); ++k)
            pts.push_back({{"position", v3(c.points[k])}, {"normalA", v3(c.normalA[k])}, {"normalB", v3(c.normalB[k])}, {"angle", c.angle[k]},
                           {"depth", c.depth[k]}, {"gap", c.gap[k]}, {"upFacing", c.upFacing[k]}, {"exposure", c.exposure[k]}, {"layer", c.layer[k]}});
        curves.push_back({{"kind", kindName(c.kind)}, {"a", objectName(d, c.a)}, {"b", objectName(d, c.b)}, {"closed", c.closed},
                          {"near", c.nearContact}, {"length", c.length()}, {"points", pts}});
    }
    json points = json::array();
    for (const SeamPoint& p : d.points)
    {
        json a = pointAttributes(d, p);
        a["position"] = v3(p.position);
        a["tangent"] = v3(p.tangent);
        points.push_back(a);
    }
    json objects = json::array();
    for (const Object& o : d.objects)
        objects.push_back(o.name);
    return {{"format", "lila-contacts"}, {"version", 1}, {"units", "cm"}, {"up", v3(d.up)}, {"objects", objects}, {"terrain", objectName(d, kTerrain)},
            {"layers", d.result->layers}, {"curves", curves}, {"points", points}};
}
} // namespace

void registerExportCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "seamExport",
        .doc = "Write a seam node's contacts to a file for other engines: .json (curves with every point's facts, points, objects; "
               "Godot and Unity read it with any JSON parser) or .usda (BasisCurves and Points with primvars). Lengths in cm.",
        .positionalDoc = "seam node",
        .flags = {{"file", "f", T::String, nullptr, "Output path, .json or .usda."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const auto names = a.objects();
            if (names.size() != 1 || !a.has("file"))
                throw CommandError("seamExport: give one seam node and file=");
            const auto d = seamDataNamed(g, names[0], "seamExport");
            const std::filesystem::path file = a.get<std::string>("file", "");
            const std::string ext = file.extension().string();
            if (ext == ".usda" || ext == ".usd")
                writeUsd(file, *d);
            else if (ext == ".json")
            {
                std::ofstream f(file);
                if (!f)
                    throw CommandError("seamExport: cannot write " + file.string());
                f << exportJson(*d).dump(1);
            }
            else
                throw CommandError("seamExport: give a .json or .usda file");
            return {{"file", file.string()}, {"curves", d->result->curves.size()}, {"points", d->points.size()}};
        },
        .examples = {"seamExport('contacts', file='C:/Game/contacts.json')", "seamExport('contacts', file='contacts.usda')"},
    });

    ctx.registerCommand({
        .name = "seamSend",
        .doc = "Send a seam node's contacts to Unreal through LilaLink: the contact materials (MF_LilaContactMask, MF_LilaSeamStrip, "
               "MF_LilaSkirtBlend and their materials), each contact as a spline actor tagged with its facts (lila.kind:ground, "
               "lila.a:Wall, lila.angle:90.00 ...), the points and splines as a PCG data asset for a PCG graph to spawn along, and "
               "each object's contactBlendId in custom primitive data for screen-space blenders. Returns what was sent.",
        .positionalDoc = "seam node",
        .flags = {{"materials", "m", T::Bool, true, "Build the contact material functions and materials."},
                  {"splines", "sp", T::Bool, true, "A spline actor per contact."},
                  {"pcg", "pc", T::Bool, true, "Points and splines as PCG data (needs the LilaLinkPCG plugin)."},
                  {"pcgAsset", "pa", T::String, nullptr, "PCG data asset path; default /Game/LilaLink/Contacts/PCG_<seam>."},
                  {"folder", "fo", T::String, "Lila/Contacts", "Outliner folder for the spline actors."},
                  {"blendIds", "bi", T::Bool, true, "Give each object a contactBlendId in custom primitive data."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::string cmd = "seamSend";
            const auto names = a.objects();
            if (names.size() != 1)
                throw CommandError(cmd + ": give one seam node");
            NodeId sid = kNoNode;
            const auto d = seamDataNamed(g, names[0], cmd, &sid);
            json out = json::object();
            if (a.get<bool>("blendIds", true))
                out["blendIds"] = assignBlendIds(g, *d);
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(cmd + ": Unreal is not connected (" + e.what() + ")");
            }
            auto client = link::Link::get().client();
            const json features = link::Link::get().status().value("features", json::array());
            auto has = [&](const char* f) { return features.is_array() && std::find(features.begin(), features.end(), json(f)) != features.end(); };
            const std::string seamName = g.node(sid)->name();
            if (a.get<bool>("materials", true))
            {
                if (!has("seams"))
                    out["materials"] = "skipped: this LilaLink has no seams feature (update the plugin)";
                else
                {
                    link::RpcResult r = client->call("seamMaterials", json::object(), link::importCallOptions());
                    if (!r.ok)
                        throw CommandError(cmd + ": seamMaterials: " + r.error);
                    out["materials"] = r.result;
                }
            }
            if (a.get<bool>("splines", true))
            {
                if (!has(link::kSplineFeature))
                    out["splines"] = "skipped: this LilaLink has no splines feature";
                else
                {
                    int sent = 0;
                    const std::string folder = a.get<std::string>("folder", "Lila/Contacts");
                    for (size_t i = 0; i < d->result->curves.size(); ++i)
                    {
                        const Curve& c = d->result->curves[i];
                        curve::Spline s;
                        s.kind = curve::Kind::Poly;
                        s.closed = c.closed && c.points.size() > 2;
                        s.points = c.points;
                        const glm::dvec3 origin = c.points.front();
                        const json params = {{"id", "LilaSeam:" + g.node(sid)->fullPath() + ":" + std::to_string(i)},
                                             {"label", seamName + "_" + std::to_string(i)},
                                             {"mode", "actor"},
                                             {"folder", folder + "/" + seamName},
                                             {"location", unrealVec(origin)},
                                             {"splines", link::splinePoints({s}, origin)},
                                             {"tags", {std::string("kind:") + kindName(c.kind), "a:" + objectName(*d, c.a), "b:" + objectName(*d, c.b),
                                                       "near:" + std::string(c.nearContact ? "1" : "0"), "angle:" + fmt(mean(c.angle)),
                                                       "depth:" + fmt(maxOf(c.depth)), "exposure:" + fmt(mean(c.exposure)), "length:" + fmt(c.length())}}};
                        link::RpcResult r = client->call("setSpline", params, 30000);
                        if (!r.ok)
                            throw CommandError(cmd + ": setSpline: " + r.error);
                        ++sent;
                    }
                    // Contacts that went away since the last send.
                    const int64_t before = intAttr(*g.node(sid), "sentSplines");
                    for (int64_t i = sent; i < before; ++i)
                        client->call("removeSpline", {{"id", "LilaSeam:" + g.node(sid)->fullPath() + ":" + std::to_string(i)}}, 30000);
                    setUserAttr(g, sid, "sentSplines", PlugType::Int, int64_t(sent));
                    out["splines"] = sent;
                }
            }
            if (a.get<bool>("pcg", true))
            {
                if (!has("pcgData"))
                    out["pcg"] = "skipped: Unreal has no LilaLinkPCG plugin (enable it for PCG data)";
                else
                {
                    const std::string asset = a.get<std::string>("pcgAsset", "/Game/LilaLink/Contacts/PCG_" + seamName);
                    out["pcg"] = link::sendPcgData(*client, features, asset, seamName, pcgSets(*d));
                    out["pcgAsset"] = asset;
                }
            }
            return out;
        },
        .examples = {"seamSend('contacts')", "seamSend('contacts', splines=False, pcgAsset='/Game/Env/PCG_WallBases')"},
    });
}
} // namespace seamwork
