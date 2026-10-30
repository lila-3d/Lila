// Tailor: the tailorGarment node, a garment (panels, seams, fabrics) kept as JSON, meshed at its particle distance
// and draped by the cloth solver onto an optional collider. Its outMesh feeds an ordinary mesh.

#include "Tailor.h"
#include "TailorCore.h"

#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Plug.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>

#include <algorithm>
#include <functional>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;

namespace tailor
{
namespace
{
constexpr double kCmPerM = 100.0;

constexpr const char* kStateNames[] = {"active", "frozen", "deactivated", "strengthened"};

json vec2Json(const Vec2& v) { return json::array({v[0], v[1]}); }
json vec3Json(const Vec3& v) { return json::array({v[0], v[1], v[2]}); }
Vec2 jsonVec2(const json& j) { return {j.at(0).get<float>(), j.at(1).get<float>()}; }
Vec3 jsonVec3(const json& j) { return {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()}; }

json polyJson(const std::vector<Vec2>& pts)
{
    json a = json::array();
    for (const Vec2& p : pts)
        a.push_back(vec2Json(p));
    return a;
}

std::vector<Vec2> jsonPoly(const json& j)
{
    std::vector<Vec2> out;
    for (const json& p : j)
        out.push_back(jsonVec2(p));
    return out;
}

json fabricJson(const Fabric& f)
{
    return {{"name", f.name},         {"stretchWarp", f.stretchWarp}, {"stretchWeft", f.stretchWeft}, {"shear", f.shear},
            {"bendWarp", f.bendWarp}, {"bendWeft", f.bendWeft},       {"bendBias", f.bendBias},       {"strainLimit", f.strainLimit},
            {"density", f.density},   {"thickness", f.thickness},     {"friction", f.friction},       {"damping", f.damping},
            {"shrinkWarp", f.shrinkWarp}, {"shrinkWeft", f.shrinkWeft}};
}

Fabric jsonFabric(const json& j)
{
    Fabric f;
    if (const Fabric* preset = findFabric(j.value("name", f.name)))
        f = *preset;
    f.name = j.value("name", f.name);
    f.stretchWarp = j.value("stretchWarp", f.stretchWarp);
    f.stretchWeft = j.value("stretchWeft", f.stretchWeft);
    f.shear = j.value("shear", f.shear);
    f.bendWarp = j.value("bendWarp", f.bendWarp);
    f.bendWeft = j.value("bendWeft", f.bendWeft);
    f.bendBias = j.value("bendBias", f.bendBias);
    f.strainLimit = j.value("strainLimit", f.strainLimit);
    f.density = j.value("density", f.density);
    f.thickness = j.value("thickness", f.thickness);
    f.friction = j.value("friction", f.friction);
    f.damping = j.value("damping", f.damping);
    f.shrinkWarp = j.value("shrinkWarp", f.shrinkWarp);
    f.shrinkWeft = j.value("shrinkWeft", f.shrinkWeft);
    return f;
}

json sidesJson(const std::vector<SeamSide>& sides)
{
    json a = json::array();
    for (const SeamSide& s : sides)
        a.push_back({{"panel", s.panel}, {"from", s.from}, {"to", s.to}});
    return a;
}

std::vector<SeamSide> jsonSides(const json& j)
{
    std::vector<SeamSide> out;
    for (const json& s : j)
        out.push_back({s.value("panel", -1), s.value("from", 0), s.value("to", 0)});
    return out;
}

/** Attr order of the tailorGarment type; it has no base, so these are the indices. */
enum GarmentAttr
{
    kGarment,
    kBlock,
    kFabric,
    kParticleDistance,
    kSimulate,
    kFrames,
    kColliderMesh,
    kColliderMatrix,
    kOutMesh,
    kWarning,
};

void hashMix(uint64_t& h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); }

/** The collider in garment space (metres), triangulated. */
Sdf colliderSdf(const geo::Mesh& m, const glm::dmat4& world)
{
    std::vector<Vec3> pts(m.pointCapacity());
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        const glm::dvec3 w = glm::dvec3(world * glm::dvec4(m.position(p), 1.0)) / kCmPerM;
        pts[p] = {float(w.x), float(w.y), float(w.z)};
    }
    std::vector<std::array<int, 3>> tris;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> c = m.faceCorners(f);
        for (size_t k = 1; k + 1 < c.size(); ++k)
            tris.push_back({int(m.cornerPoint(c[0])), int(m.cornerPoint(c[k])), int(m.cornerPoint(c[k + 1]))});
    }
    return sdfFromMesh(pts, tris, 0.01f, 0.05f);
}

MeshPtr clothToMesh(const ClothMesh& cm, const std::vector<Vec3>& positions)
{
    std::vector<glm::dvec3> pts(positions.size());
    for (size_t k = 0; k < positions.size(); ++k)
        pts[k] = glm::dvec3(positions[k][0], positions[k][1], positions[k][2]) * kCmPerM;
    std::vector<geo::Index> sizes(cm.tris.size(), 3), idx;
    idx.reserve(cm.tris.size() * 3);
    for (const auto& t : cm.tris)
        for (int v : t)
            idx.push_back(geo::Index(v));
    auto mesh = std::make_shared<geo::Mesh>(geo::Mesh::fromPolygons(pts, sizes, idx));

    // UVs are the flat pattern, one UV unit per metre.
    std::span<glm::vec2> uv = mesh->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    std::span<int32_t> panel = mesh->faceAttrs().getOrAdd<int32_t>("panel");
    for (geo::Index f = 0; f < mesh->faceCapacity(); ++f)
    {
        if (!mesh->faceValid(f))
            continue;
        if (f < cm.triPanel.size())
            panel[f] = cm.triPanel[f];
        for (geo::Index c : mesh->faceCorners(f))
            if (const geo::Index p = mesh->cornerPoint(c); p < cm.uv.size())
                uv[c] = glm::vec2(cm.uv[p][0], cm.uv[p][1]);
    }
    return mesh;
}

class GarmentNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        std::string warning;
        try
        {
            setOutput(kOutMesh, build());
        }
        catch (const std::exception& e)
        {
            warning = e.what();
            setOutput(kOutMesh, MeshPtr());
        }
        if (!warning.empty() && warning != as<std::string>(kWarning))
            log::warn("{}: {}", name(), warning);
        setOutput(kWarning, warning);
    }

private:
    MeshPtr build()
    {
        const std::string& text = as<std::string>(kGarment);
        if (text.empty())
            return MeshPtr();
        Garment g = garmentFromJson(json::parse(text));
        if (const std::string& preset = as<std::string>(kFabric); !preset.empty())
        {
            const Fabric* f = findFabric(preset);
            if (!f)
                throw std::runtime_error("unknown fabric preset '" + preset + "'");
            g.fabrics.at(0) = *f;
        }
        if (const double pd = as<double>(kParticleDistance); pd > 0.0)
            for (Panel& p : g.panels)
                p.particleDistance = float(pd / kCmPerM);

        const ClothMesh cm = buildClothMesh(g);
        const int64_t frames = as<int64_t>(kFrames);
        if (!as<bool>(kSimulate) || frames <= 0 || cm.rest3d.empty())
            return clothToMesh(cm, cm.rest3d);

        const MeshPtr& collider = as<MeshPtr>(kColliderMesh);
        const glm::dmat4& colliderWorld = as<glm::dmat4>(kColliderMatrix);
        uint64_t key = std::hash<std::string>{}(text);
        hashMix(key, std::hash<std::string>{}(as<std::string>(kFabric)));
        hashMix(key, std::hash<double>{}(as<double>(kParticleDistance)));
        hashMix(key, uint64_t(frames));
        hashMix(key, uint64_t(uintptr_t(collider.get())));
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                hashMix(key, std::hash<double>{}(colliderWorld[c][r]));

        if (key != mDrapeKey || mDrape.size() != cm.rest3d.size())
        {
            Sdf sdf;
            ClothSolver solver;
            solver.reset(cm, g, SolverSettings{});
            if (collider && !collider->empty())
            {
                sdf = colliderSdf(*collider, colliderWorld);
                solver.setColliders({&sdf});
            }
            for (int64_t k = 0; k < frames; ++k)
                solver.step();
            mDrape = solver.positions();
            mDrapeKey = key;
        }
        return clothToMesh(cm, mDrape);
    }

    uint64_t mDrapeKey = 0;
    std::vector<Vec3> mDrape;
};

constexpr uint32_t kGarmentIn = AttrInput | AttrStorable;

NodeType garmentType()
{
    NodeType t;
    t.name = kGarmentType;
    t.icon = "shirt";
    t.doc = "A garment: pattern panels sewn by seams, meshed at a particle distance and draped onto a collider. Feeds a mesh's inMesh.";
    t.ownAttrs = {
        {.name = "garment", .type = PlugType::String, .defaultValue = std::string(), .flags = kGarmentIn | AttrHidden | AttrMultiline,
         .doc = "Panels, seams and fabrics, JSON. Pattern space and placement are in metres."},
        {.name = "block", .type = PlugType::String, .defaultValue = std::string(), .flags = kGarmentIn,
         .doc = "The block the garment was started from, if any.", .category = "Garment"},
        {.name = "fabric", .type = PlugType::String, .defaultValue = std::string(), .flags = kGarmentIn,
         .doc = "Fabric preset for panels on the garment's first fabric; empty keeps the garment's own.", .category = "Garment"},
        {.name = "particleDistance", .type = PlugType::Float, .defaultValue = 2.0, .flags = kGarmentIn | AttrKeyable,
         .doc = "Target cloth edge length, cm; 0 uses each panel's own.", .minValue = 0.0, .softMin = 0.3, .softMax = 5.0, .category = "Garment"},
        {.name = "simulate", .type = PlugType::Bool, .defaultValue = true, .flags = kGarmentIn,
         .doc = "Drape the garment; off shows the panels where they were placed.", .category = "Simulation"},
        {.name = "frames", .type = PlugType::Int, .defaultValue = int64_t(90), .flags = kGarmentIn,
         .doc = "Solver frames run to settle the drape.", .minValue = 0.0, .softMax = 600.0, .category = "Simulation"},
        {.name = "colliderMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden,
         .doc = "The body the garment drapes onto: connect a mesh's outMesh."},
        {.name = "colliderMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
         .doc = "The collider's world matrix: connect its worldMatrix."},
        {.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "The draped garment, cm, with the flat pattern as UVs and a face int attribute panel."},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput,
         .doc = "Problems with the last build."},
    };
    t.create = [] { return std::make_unique<GarmentNode>(); };
    return t;
}
} // namespace

const char* panelStateName(PanelState s) { return kStateNames[size_t(s)]; }

bool parsePanelState(const std::string& name, PanelState* out)
{
    for (size_t k = 0; k < std::size(kStateNames); ++k)
        if (name == kStateNames[k])
        {
            *out = PanelState(k);
            return true;
        }
    return false;
}

json garmentToJson(const Garment& g)
{
    json panels = json::array();
    for (const Panel& p : g.panels)
    {
        json holes = json::array(), lines = json::array();
        for (const auto& h : p.holes)
            holes.push_back(polyJson(h));
        for (const auto& l : p.internalLines)
            lines.push_back(polyJson(l));
        panels.push_back({{"name", p.name},
                          {"outline", polyJson(p.outline)},
                          {"holes", holes},
                          {"internalLines", lines},
                          {"grainAngle", p.grainAngle},
                          {"fabric", p.fabric},
                          {"layer", p.layer},
                          {"state", panelStateName(p.state)},
                          {"particleDistance", p.particleDistance},
                          {"origin", vec3Json(p.origin)},
                          {"right", vec3Json(p.right)},
                          {"up", vec3Json(p.up)},
                          {"wrapRadius", p.wrapRadius}});
    }
    json seams = json::array();
    for (const Seam& s : g.seams)
        seams.push_back({{"a", sidesJson(s.a)}, {"b", sidesJson(s.b)}, {"reversed", s.reversed}, {"foldAngle", s.foldAngle}, {"shrink", s.shrink}});
    json fabrics = json::array();
    for (const Fabric& f : g.fabrics)
        fabrics.push_back(fabricJson(f));
    return {{"version", 1}, {"panels", panels}, {"seams", seams}, {"fabrics", fabrics}};
}

Garment garmentFromJson(const json& j)
{
    Garment g;
    for (const json& pj : j.value("panels", json::array()))
    {
        Panel p;
        p.name = pj.value("name", std::string());
        p.outline = jsonPoly(pj.value("outline", json::array()));
        for (const json& h : pj.value("holes", json::array()))
            p.holes.push_back(jsonPoly(h));
        for (const json& l : pj.value("internalLines", json::array()))
            p.internalLines.push_back(jsonPoly(l));
        p.grainAngle = pj.value("grainAngle", p.grainAngle);
        p.fabric = pj.value("fabric", p.fabric);
        p.layer = pj.value("layer", p.layer);
        if (!parsePanelState(pj.value("state", std::string("active")), &p.state))
            throw std::runtime_error("panel '" + p.name + "': unknown state");
        p.particleDistance = pj.value("particleDistance", p.particleDistance);
        if (pj.contains("origin"))
            p.origin = jsonVec3(pj["origin"]);
        if (pj.contains("right"))
            p.right = jsonVec3(pj["right"]);
        if (pj.contains("up"))
            p.up = jsonVec3(pj["up"]);
        p.wrapRadius = pj.value("wrapRadius", p.wrapRadius);
        g.panels.push_back(std::move(p));
    }
    for (const json& sj : j.value("seams", json::array()))
    {
        Seam s;
        s.a = jsonSides(sj.value("a", json::array()));
        s.b = jsonSides(sj.value("b", json::array()));
        s.reversed = sj.value("reversed", s.reversed);
        s.foldAngle = sj.value("foldAngle", s.foldAngle);
        s.shrink = sj.value("shrink", s.shrink);
        g.seams.push_back(std::move(s));
    }
    if (j.contains("fabrics") && !j["fabrics"].empty())
    {
        g.fabrics.clear();
        for (const json& fj : j["fabrics"])
            g.fabrics.push_back(jsonFabric(fj));
    }
    for (Panel& p : g.panels)
        p.fabric = std::clamp(p.fabric, 0, int(g.fabrics.size()) - 1);
    return g;
}

bool registerNodes(ext::Context& ctx)
{
    relocate::registerNeutral(kGarmentType);
    return types::registerType(ctx, garmentType()) != nullptr;
}
} // namespace tailor

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!tailor::registerNodes(ctx))
        return 1;
    tailor::registerCommands(ctx);
    tailor::registerUi(ctx);
    return 0;
}
