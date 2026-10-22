#include "gn/Geometry.h"

#include "core/Profile.h"

#include <tbb/parallel_for.h>

#include <array>

namespace lila::gn
{
namespace
{
constexpr std::array<const char*, 7> kSocketNames = {"geometry", "float", "int", "bool", "vector", "rotation", "string"};
constexpr std::array<const char*, 6> kDomainNames = {"point", "edge", "face", "corner", "curve", "instance"};
} // namespace

const char* socketTypeName(SocketType t) { return kSocketNames[size_t(t)]; }

bool parseSocketType(std::string_view name, SocketType* out)
{
    for (size_t i = 0; i < kSocketNames.size(); ++i)
        if (name == kSocketNames[i])
        {
            *out = SocketType(i);
            return true;
        }
    return false;
}

bool isDataType(SocketType t) { return t != SocketType::Geometry && t != SocketType::String; }

const char* domainName(Domain d) { return kDomainNames[size_t(d)]; }

bool parseDomain(std::string_view name, Domain* out)
{
    for (size_t i = 0; i < kDomainNames.size(); ++i)
        if (name == kDomainNames[i])
        {
            *out = Domain(i);
            return true;
        }
    return false;
}

void CurveSet::addCurve(std::span<const glm::dvec3> points, bool closed)
{
    positions.insert(positions.end(), points.begin(), points.end());
    offsets.push_back(geo::Index(positions.size()));
    cyclic.push_back(closed ? 1 : 0);
    syncSizes();
}

void CurveSet::syncSizes()
{
    pointAttrs.resize(positions.size());
    curveAttrs.resize(cyclic.size());
}

void CurveSet::append(const CurveSet& other)
{
    pointAttrs.mergeLayout(other.pointAttrs);
    curveAttrs.mergeLayout(other.curveAttrs);
    size_t p0 = positions.size(), c0 = cyclic.size();
    positions.insert(positions.end(), other.positions.begin(), other.positions.end());
    for (size_t c = 0; c < other.numCurves(); ++c)
        offsets.push_back(geo::Index(p0 + other.offsets[c + 1]));
    cyclic.insert(cyclic.end(), other.cyclic.begin(), other.cyclic.end());
    syncSizes();
    for (size_t p = 0; p < other.numPoints(); ++p)
        pointAttrs.copyFrom(other.pointAttrs, p0 + p, p);
    for (size_t c = 0; c < other.numCurves(); ++c)
        curveAttrs.copyFrom(other.curveAttrs, c0 + c, c);
}

void Instances::add(const glm::dmat4& xf, int ref)
{
    transforms.push_back(xf);
    reference.push_back(ref);
    attrs.resize(transforms.size());
}

bool Geometry::empty() const
{
    return (!mesh || mesh->numPoints() == 0) && (!points || points->numPoints() == 0) && (!curves || curves->numPoints() == 0) &&
           (!instances || instances->size() == 0);
}

GeometryPtr makeGeometry(Geometry g) { return std::make_shared<const Geometry>(std::move(g)); }

GeometryPtr meshGeometry(geo::Mesh m)
{
    m.compact();
    Geometry g;
    g.mesh = std::make_shared<const geo::Mesh>(std::move(m));
    return makeGeometry(std::move(g));
}

GeometryPtr pointGeometry(geo::Mesh m)
{
    Geometry g;
    g.points = std::make_shared<const geo::Mesh>(std::move(m));
    return makeGeometry(std::move(g));
}

GeometryPtr curveGeometry(CurveSet c)
{
    Geometry g;
    g.curves = std::make_shared<const CurveSet>(std::move(c));
    return makeGeometry(std::move(g));
}

void transformMesh(geo::Mesh& m, const glm::dmat4& xf)
{
    LILA_ZONE();

    std::span<glm::dvec3> pos = m.positions();
    tbb::parallel_for(size_t(0), pos.size(), [&](size_t p) { pos[p] = glm::dvec3(xf * glm::dvec4(pos[p], 1.0)); });
    glm::mat3 nm = glm::mat3(glm::transpose(glm::inverse(glm::dmat3(xf))));
    for (auto& n : m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal))
        n = glm::normalize(nm * n);
    if (glm::determinant(glm::dmat3(xf)) < 0.0)
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                m.flipFace(f);
}

void transformCurves(CurveSet& c, const glm::dmat4& xf)
{
    for (glm::dvec3& p : c.positions)
        p = glm::dvec3(xf * glm::dvec4(p, 1.0));
}

Geometry transformed(const Geometry& g, const glm::dmat4& xf)
{
    LILA_ZONE();

    Geometry out;
    if (g.mesh)
    {
        geo::Mesh m = *g.mesh;
        transformMesh(m, xf);
        out.mesh = std::make_shared<const geo::Mesh>(std::move(m));
    }
    if (g.points)
    {
        geo::Mesh m = *g.points;
        transformMesh(m, xf);
        out.points = std::make_shared<const geo::Mesh>(std::move(m));
    }
    if (g.curves)
    {
        CurveSet c = *g.curves;
        transformCurves(c, xf);
        out.curves = std::make_shared<const CurveSet>(std::move(c));
    }
    if (g.instances)
    {
        Instances i = *g.instances;
        for (glm::dmat4& m : i.transforms)
            m = xf * m;
        out.instances = std::make_shared<const Instances>(std::move(i));
    }
    return out;
}

Geometry join(std::span<const GeometryPtr> parts)
{
    LILA_ZONE();

    Geometry out;
    geo::Mesh mesh, points;
    CurveSet curves;
    Instances inst;
    bool hasMesh = false, hasPoints = false, hasCurves = false, hasInst = false;
    for (const GeometryPtr& g : parts)
    {
        if (!g)
            continue;
        if (g->mesh)
        {
            // A single mesh is shared, not copied.
            if (!hasMesh && !out.mesh)
                out.mesh = g->mesh;
            else
            {
                if (!hasMesh)
                {
                    mesh = *out.mesh;
                    hasMesh = true;
                }
                mesh.append(*g->mesh);
            }
        }
        if (g->points)
        {
            points.append(*g->points);
            hasPoints = true;
        }
        if (g->curves)
        {
            curves.append(*g->curves);
            hasCurves = true;
        }
        if (g->instances)
        {
            const Instances& o = *g->instances;
            inst.attrs.mergeLayout(o.attrs);
            int refBase = int(inst.references.size());
            inst.references.insert(inst.references.end(), o.references.begin(), o.references.end());
            size_t first = inst.size();
            for (size_t k = 0; k < o.size(); ++k)
                inst.add(o.transforms[k], o.reference[k] + refBase);
            for (size_t k = 0; k < o.size(); ++k)
                inst.attrs.copyFrom(o.attrs, first + k, k);
            hasInst = true;
        }
    }
    if (hasMesh)
    {
        mesh.compact();
        out.mesh = std::make_shared<const geo::Mesh>(std::move(mesh));
    }
    if (hasPoints)
        out.points = std::make_shared<const geo::Mesh>(std::move(points));
    if (hasCurves)
        out.curves = std::make_shared<const CurveSet>(std::move(curves));
    if (hasInst)
        out.instances = std::make_shared<const Instances>(std::move(inst));
    return out;
}

Geometry realize(const Geometry& g)
{
    LILA_ZONE();

    if (!g.instances || g.instances->size() == 0)
    {
        Geometry out = g;
        out.instances = nullptr;
        return out;
    }
    const Instances& inst = *g.instances;
    // Each reference is realized once, then copied per instance.
    std::vector<Geometry> refs(inst.references.size());
    for (size_t r = 0; r < refs.size(); ++r)
        if (inst.references[r])
            refs[r] = realize(*inst.references[r]);

    std::vector<GeometryPtr> parts;
    parts.reserve(inst.size() + 1);
    Geometry self = g;
    self.instances = nullptr;
    parts.push_back(makeGeometry(self));
    std::vector<GeometryPtr> placed(inst.size());
    tbb::parallel_for(size_t(0), inst.size(), [&](size_t k) {
        int r = inst.reference[k];
        if (r >= 0 && size_t(r) < refs.size())
            placed[k] = makeGeometry(transformed(refs[r], inst.transforms[k]));
    });
    parts.insert(parts.end(), placed.begin(), placed.end());
    return join(parts);
}

geo::Mesh pointsOnly(const geo::Mesh& m)
{
    geo::Mesh out;
    out.pointAttrs().mergeLayout(m.pointAttrs());
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            geo::Index np = out.addPoint(m.position(p));
            out.pointAttrs().copyFrom(m.pointAttrs(), np, p);
        }
    return out;
}

void stripAnonymous(geo::Mesh& m)
{
    for (size_t d = 0; d < size_t(geo::Domain::Count); ++d)
    {
        geo::AttributeSet& set = m.attributes(geo::Domain(d));
        for (const std::string& n : set.names())
            if (n.starts_with("__"))
                set.remove(n);
    }
}

geo::Mesh toOutputMesh(const Geometry& g)
{
    LILA_ZONE();

    Geometry r = realize(g);
    geo::Mesh m = r.mesh ? *r.mesh : geo::Mesh();
    stripAnonymous(m);
    m.compact();
    return m;
}
} // namespace lila::gn
