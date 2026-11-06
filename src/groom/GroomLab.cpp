#include "groom/GroomLab.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/NodeHelpers.h"
#include "geo/Mesh.h"
#include "groom/Attrs.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <groomlab/graph.h>
#include <groomlab/graph_io.h>
#include <groomlab/groom.h>
#include <groomlab/guide_set.h>
#include <groomlab/scalp_mesh.h>
#include <groomlab/strand_set.h>

#include <map>
#include <memory>
#include <stdexcept>

namespace lila::groom
{
namespace groomLabBridge
{
groomlab::Vec3f toVec(const glm::dvec3& p)
{
    return {float(p.x), float(p.y), float(p.z)};
}

/** The scalp's faces fanned into triangles over its live points, renumbered densely. */
std::shared_ptr<const groomlab::ScalpMesh> scalpMesh(const geo::Mesh& mesh)
{
    std::vector<groomlab::Vec3f> points;
    std::vector<int32_t> remap(mesh.pointCapacity(), -1);
    for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
        {
            remap[p] = int32_t(points.size());
            points.push_back(toVec(mesh.position(p)));
        }
    std::vector<groomlab::ScalpMesh::Triangle> triangles;
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const std::vector<geo::Index> v = mesh.facePoints(f);
        for (size_t i = 1; i + 1 < v.size(); ++i)
            triangles.push_back({remap[v[0]], remap[v[i]], remap[v[i + 1]]});
    }
    return std::make_shared<groomlab::ScalpMesh>(std::move(points), std::move(triangles));
}

std::shared_ptr<const groomlab::GuideSet> guideSet(const Splines& guides, const groomlab::ScalpMesh& scalp)
{
    auto set = std::make_shared<groomlab::GuideSet>();
    uint64_t next = 1;
    for (const Spline& s : guides)
    {
        if (s.size() < 2)
            continue;
        std::vector<groomlab::Vec3f> points;
        points.reserve(s.size());
        for (const glm::dvec3& p : s.points)
            points.push_back(toVec(p));
        set->add(next++, std::move(points));
    }
    set->rootOnMesh(scalp);
    return set;
}

Splines toSplines(const groomlab::StrandSet& strands)
{
    Splines out(strands.strandCount());
    const std::vector<float>& widths = strands.widths();
    const bool hasWidths = widths.size() == strands.pointCount();
    for (size_t i = 0; i < out.size(); ++i)
    {
        Spline& s = out[i];
        s.kind = Kind::Poly;
        const size_t n = strands.strandPointCount(i), begin = strands.strandBegin(i);
        const groomlab::Vec3f* p = strands.strandPoints(i);
        s.points.reserve(n);
        for (size_t k = 0; k < n; ++k)
            s.points.push_back({p[k].x, p[k].y, p[k].z});
        if (hasWidths)
            s.pointAttrs[std::string(attr::kWidth)].assign(widths.begin() + ptrdiff_t(begin), widths.begin() + ptrdiff_t(begin + n));
        s.attrs[std::string(attr::kId)] = double(strands.ids()[i]);
    }
    return out;
}
} // namespace groomLabBridge

GrownStrands growStrands(const geo::Mesh& scalp, const Splines& guides, const std::string& graphJson, double seconds)
{
    LILA_ZONE();
    using namespace groomLabBridge;
    groomlab::GraphDocument doc = [&] {
        try
        {
            return groomlab::loadGraphJson(graphJson.empty() ? groomlab::defaultGraphJson() : graphJson);
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error(std::string("groom graph: ") + e.what());
        }
    }();
    const std::shared_ptr<const groomlab::ScalpMesh> mesh = scalpMesh(scalp);
    groomlab::ExternalInputs externals;
    groomlab::setGroomExternals(externals, mesh, guides.empty() ? nullptr : guideSet(guides, *mesh), nullptr);
    groomlab::EvalContext ctx;
    ctx.externals = &externals;
    ctx.time = seconds;

    const groomlab::Value result = doc.graph.evaluate(doc.output, ctx);
    GrownStrands out;
    if (const groomlab::StrandsPtr strands = groomlab::valueAs<groomlab::StrandsPtr>(result))
        out.strands = toSplines(*strands);
    else if (const groomlab::GuidesPtr g = groomlab::valueAs<groomlab::GuidesPtr>(result))
    {
        groomlab::StrandSet asStrands;
        for (const groomlab::Guide& guide : g->guides())
            asStrands.addStrand(guide.id, guide.points, {}, guide.root);
        out.strands = toSplines(asStrands);
    }
    else
        throw std::runtime_error("groom graph: its output makes neither strands nor guides");

    std::map<groomlab::NodeId, std::string> names;
    for (const auto& [name, id] : doc.nodesByName)
        names[id] = name;
    for (const auto& [id, note] : doc.graph.lastNotes())
        out.warnings.push_back(names[id] + ": " + note);
    return out;
}

namespace
{
using namespace scene;
using namespace curve::helpers;

/** Strands grown on the scalp by a GroomLab graph, guided by the incoming curves. */
class GroomStrandsNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();
        const MeshPtr& m = as<MeshPtr>(idx(*this, "inMesh"));
        std::string warning;
        Splines strands;
        if (m && m->numFaces() > 0)
        {
            try
            {
                geo::Mesh world = *m;
                const glm::dmat4& mm = as<glm::dmat4>(idx(*this, "meshMatrix"));
                for (geo::Index p = 0; p < world.pointCapacity(); ++p)
                    if (world.pointValid(p))
                        world.setPosition(p, D3(mm * glm::dvec4(world.position(p), 1.0)));
                GrownStrands grown = growStrands(world, curve::splinesOf(value(idx(*this, "inCurve"))), s(*this, "graph"));
                strands = std::move(grown.strands);
                for (const std::string& w : grown.warnings)
                    warning += (warning.empty() ? "" : "; ") + w;
            }
            catch (const std::exception& e)
            {
                warning = e.what();
            }
        }
        if (!warning.empty() && warning != as<std::string>(idx(*this, "warning")))
            log::warn("{}: {}", name(), warning);
        setOutput(idx(*this, "strandCount"), int64_t(strands.size()));
        setOutput(idx(*this, "outCurve"), curve::makeCurveData(std::move(strands)));
        setOutput(idx(*this, "warning"), warning);
    }
};
} // namespace

void registerGroomLabType()
{
    NodeType t;
    t.name = "groomStrands";
    t.icon = "feather";
    t.doc = "Strands grown on the scalp by a groom graph (scatter, interpolate, clump, curl, noise and the rest), guided by "
            "the incoming curves.";
    const std::string c = "Strands";
    t.ownAttrs = {
        {.name = "inCurve", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = kIn | AttrHidden, .doc = "Guides, world space."},
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden, .doc = "The scalp."},
        {.name = "meshMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden, .doc = "The scalp's world matrix."},
        sAttrDefault("graph", "", "The groom graph as JSON; empty grows the default hair.", c),
        dataOut("outCurve", "The strands, world space, with a width per point."),
        {.name = "strandCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Strands grown."},
        stringOut("warning", "Problems with the last evaluation."),
    };
    t.create = [] { return std::make_unique<GroomStrandsNode>(); };
    types::registerType(std::move(t));
}
} // namespace lila::groom
