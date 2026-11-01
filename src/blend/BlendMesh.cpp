#include "blend/BlendInternal.h"

#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "scene/Materials.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace lila::blend
{
namespace
{
/** CustomData layer types (eCustomDataType) as attribute types, -1 for the ones not read as attributes. */
int attrTypeOfLayer(int cd)
{
    switch (cd)
    {
    case 10: return 5;  // CD_PROP_FLOAT
    case 11: return 3;  // CD_PROP_INT32
    case 17: return 9;  // CD_PROP_BYTE_COLOR
    case 45: return 1;  // CD_PROP_INT8
    case 46: return 4;  // CD_PROP_INT32_2D
    case 47: return 10; // CD_PROP_COLOR
    case 48: return 7;  // CD_PROP_FLOAT3
    case 49: return 6;  // CD_PROP_FLOAT2
    case 50: return 0;  // CD_PROP_BOOL
    case 52: return 11; // CD_PROP_QUATERNION
    default: return -1;
    }
}

float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

template <class T>
T value(const MeshAttr& a, size_t i, T fallback = T())
{
    const uint8_t* p = a.at(i);
    if (!p)
        return fallback;
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

glm::vec4 colorOf(const MeshAttr& a, size_t i)
{
    if (a.type == 9)
    {
        const uint8_t* p = a.at(i);
        if (!p)
            return glm::vec4(1.0f);
        return {srgbToLinear(p[0] / 255.0f), srgbToLinear(p[1] / 255.0f), srgbToLinear(p[2] / 255.0f), p[3] / 255.0f};
    }
    return value<glm::vec4>(a, i, glm::vec4(1.0f));
}

bool isInternal(const std::string& name)
{
    return name.empty() || name[0] == '.';
}

glm::dvec3 faceNormal(const geo::Mesh& m, geo::Index f)
{
    glm::dvec3 n(0.0);
    const geo::Index first = m.faceCorner(f);
    geo::Index c = first;
    do
    {
        const glm::dvec3 a = m.position(m.cornerPoint(c)), b = m.position(m.cornerNextPoint(c));
        n += glm::dvec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
        c = m.cornerNext(c);
    } while (c != first);
    const double len = glm::length(n);
    return len > 0.0 ? n / len : n;
}
} // namespace

size_t MeshAttr::typeSize(int type)
{
    switch (type)
    {
    case 0:
    case 1: return 1;
    case 2: return 4;
    case 3: return 4;
    case 4: return 8;
    case 5: return 4;
    case 6: return 8;
    case 7: return 12;
    case 8: return 64;
    case 9: return 4;
    case 10: return 16;
    case 11: return 16;
    default: return 0;
    }
}

std::vector<MeshAttr> Importer::meshAttributes(const View& mesh) const
{
    std::vector<MeshAttr> out;
    if (mesh.has("attribute_storage"))
    {
        const View storage = mesh.sub("attribute_storage");
        for (const View& a : storage.array("dna_attributes", size_t(std::max(0, storage.i("dna_attributes_num"))), "Attribute"))
        {
            MeshAttr m;
            m.name = a.cstr("name");
            m.type = a.i("data_type");
            m.domain = a.i("domain");
            const View holder = a.deref("data");
            const Block* values = holder ? holder.block("data") : nullptr;
            if (!values)
                continue;
            m.single = a.i("storage_type") == 1;
            m.data = values->data;
            m.bytes = values->size;
            out.push_back(std::move(m));
        }
    }
    static constexpr const char* kData[] = {"vdata", "edata", "pdata", "ldata"};
    for (int domain = 0; domain < 4; ++domain)
    {
        const View cd = mesh.sub(kData[domain]);
        if (!cd)
            continue;
        for (const View& layer : cd.array("layers", size_t(std::max(0, cd.i("totlayer"))), "CustomDataLayer"))
        {
            const int type = attrTypeOfLayer(layer.i("type"));
            const Block* values = layer.block("data");
            const std::string name = layer.str("name");
            if (type < 0 || !values || std::any_of(out.begin(), out.end(), [&](const MeshAttr& a) { return a.name == name && a.domain == domain; }))
                continue;
            out.push_back({.name = name, .type = type, .domain = domain, .data = values->data, .bytes = values->size});
        }
    }
    return out;
}

std::shared_ptr<geo::Mesh> Importer::buildMesh(const View& me, const View& object)
{
    LILA_ZONE();

    using geo::Index;
    const std::string meshName = me.idName();
    const std::vector<MeshAttr> attrs = meshAttributes(me);
    auto find = [&](std::string_view name, int domain, int type = -1) -> const MeshAttr* {
        for (const MeshAttr& a : attrs)
            if (a.name == name && a.domain == domain && (type < 0 || a.type == type))
                return &a;
        return nullptr;
    };
    const size_t nV = size_t(std::max(0, me.i("totvert"))), nE = size_t(std::max(0, me.i("totedge"))),
                 nF = size_t(std::max(0, me.i("totpoly", 0, me.i("faces_num")))), nL = size_t(std::max(0, me.i("totloop", 0, me.i("corners_num"))));
    const MeshAttr* position = find("position", 0, 7);
    const MeshAttr* cornerVert = find(".corner_vert", 3, 3);
    const std::span<const int32_t> offsets = me.raw<int32_t>("poly_offset_indices");
    auto m = std::make_shared<geo::Mesh>();
    if (nV == 0)
        return m;
    if (!position || !position->covers(nV) || (nF > 0 && (!cornerVert || !cornerVert->covers(nL) || offsets.size() < nF + 1)))
    {
        if (me.ptr("mpoly") || me.ptr("mvert"))
            r.skipped.push_back(std::format("mesh '{}': saved by a Blender older than 3.6; open and save the file in a current Blender", meshName));
        else
            r.warnings.push_back(std::format("mesh '{}': its geometry could not be read", meshName));
        return nullptr;
    }

    m->reserve(nV, nE, nF, nL);
    for (size_t v = 0; v < nV; ++v)
        m->addPoint(axes.point(glm::dvec3(value<glm::vec3>(*position, v))));
    std::vector<Index> faceOf(nF, geo::kInvalid);
    std::vector<Index> pts;
    size_t failed = 0;
    for (size_t f = 0; f < nF; ++f)
    {
        const int32_t a = offsets[f], b = offsets[f + 1];
        if (a < 0 || b <= a || size_t(b) > nL)
        {
            ++failed;
            continue;
        }
        pts.clear();
        for (int32_t c = a; c < b; ++c)
            pts.push_back(Index(value<int32_t>(*cornerVert, size_t(c))));
        if (std::any_of(pts.begin(), pts.end(), [&](Index p) { return p >= nV; }))
        {
            ++failed;
            continue;
        }
        faceOf[f] = m->addFace(pts);
        if (faceOf[f] == geo::kInvalid)
            ++failed;
    }
    if (failed)
        r.warnings.push_back(std::format("mesh '{}': {} face(s) Lila couldn't build (degenerate or non-manifold) were skipped", meshName, failed));

    // Lila corner of each Blender corner.
    std::vector<Index> cornerOf(nL, geo::kInvalid);
    for (size_t f = 0; f < nF; ++f)
    {
        if (faceOf[f] == geo::kInvalid)
            continue;
        Index c = m->findCorner(faceOf[f], Index(value<int32_t>(*cornerVert, size_t(offsets[f]))));
        for (int32_t k = offsets[f]; k < offsets[f + 1] && c != geo::kInvalid; ++k, c = m->cornerNext(c))
            cornerOf[size_t(k)] = c;
    }

    // Edges: Blender's edge index to Lila's, adding loose ones.
    std::vector<Index> edgeOf(nE, geo::kInvalid);
    if (const MeshAttr* ev = find(".edge_verts", 1, 4); ev && ev->covers(nE))
        for (size_t e = 0; e < nE; ++e)
        {
            const glm::ivec2 v = value<glm::ivec2>(*ev, e);
            if (size_t(v.x) >= nV || size_t(v.y) >= nV || v.x == v.y)
                continue;
            edgeOf[e] = m->findEdge(Index(v.x), Index(v.y));
            if (edgeOf[e] == geo::kInvalid)
                edgeOf[e] = m->addEdge(Index(v.x), Index(v.y));
        }

    // Materials.
    const std::vector<std::string> slots = materialSlots(object, me);
    if (!slots.empty())
    {
        m->materialNames() = slots;
        const MeshAttr* index = find("material_index", 2, 3);
        std::span<int32_t> mat = m->faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
        for (size_t f = 0; f < nF; ++f)
            if (faceOf[f] != geo::kInvalid)
                mat[faceOf[f]] = index ? std::clamp<int32_t>(value<int32_t>(*index, f), 0, int32_t(slots.size()) - 1) : 0;
    }

    // Hard edges: sharp edges plus every edge of a flat face.
    {
        const MeshAttr* sharpEdge = find("sharp_edge", 1, 0);
        const MeshAttr* sharpFace = find("sharp_face", 2, 0);
        std::span<bool> hard = m->edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
        if (sharpEdge)
            for (size_t e = 0; e < nE; ++e)
                if (edgeOf[e] != geo::kInvalid && value<uint8_t>(*sharpEdge, e))
                    hard[edgeOf[e]] = true;
        if (sharpFace)
            for (size_t f = 0; f < nF; ++f)
                if (faceOf[f] != geo::kInvalid && value<uint8_t>(*sharpFace, f))
                {
                    const Index first = m->faceCorner(faceOf[f]);
                    Index c = first;
                    do
                    {
                        hard[m->cornerEdge(c)] = true;
                        c = m->cornerNext(c);
                    } while (c != first);
                }
        smoothByAngle(object, me, *m);
    }

    std::set<std::string> handled = {"position", "material_index", "sharp_edge", "sharp_face", "custom_normal"};
    if (const MeshAttr* crease = find("crease_edge", 1, 5))
    {
        handled.insert(crease->name);
        std::span<float> c = m->edgeAttrs().getOrAdd<float>(geo::attr::kCrease);
        for (size_t e = 0; e < nE; ++e)
            if (edgeOf[e] != geo::kInvalid)
                c[edgeOf[e]] = value<float>(*crease, e);
    }
    if (const MeshAttr* bevel = find("bevel_weight_edge", 1, 5))
    {
        handled.insert(bevel->name);
        std::span<float> b = m->edgeAttrs().getOrAdd<float>(geo::attr::kBevelWeight);
        for (size_t e = 0; e < nE; ++e)
            if (edgeOf[e] != geo::kInvalid)
                b[edgeOf[e]] = value<float>(*bevel, e);
    }
    for (const char* seamName : {".uv_seam", "uv_seam"})
        if (const MeshAttr* seam = find(seamName, 1, 0))
        {
            std::span<bool> s = m->edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
            for (size_t e = 0; e < nE; ++e)
                if (edgeOf[e] != geo::kInvalid && value<uint8_t>(*seam, e))
                    s[edgeOf[e]] = true;
        }

    // UV maps: float2 corner attributes, the render-default one first.
    std::vector<const MeshAttr*> uvs;
    for (const MeshAttr& a : attrs)
        if (a.domain == 3 && a.type == 6 && !isInternal(a.name) && a.covers(nL))
            uvs.push_back(&a);
    const std::string defaultUv = me.cstr("default_uv_map_attribute");
    std::stable_partition(uvs.begin(), uvs.end(), [&](const MeshAttr* a) { return a->name == defaultUv; });
    for (size_t s = 0; s < uvs.size(); ++s)
    {
        handled.insert(uvs[s]->name);
        std::span<glm::vec2> uv = m->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(int(s)));
        for (size_t k = 0; k < nL; ++k)
            if (cornerOf[k] != geo::kInvalid)
                uv[cornerOf[k]] = value<glm::vec2>(*uvs[s], k);
    }

    // Color attributes, the render-default one first.
    const std::string defaultColor = me.cstr("default_color_attribute");
    std::string firstSet;
    for (const MeshAttr& a : attrs)
    {
        if ((a.type != 9 && a.type != 10) || (a.domain != 0 && a.domain != 3) || isInternal(a.name) || !a.covers(a.domain == 0 ? nV : nL))
            continue;
        handled.insert(a.name);
        const std::string set = geo::colors::has(*m, a.name) ? geo::colors::uniqueName(*m, a.name) : a.name;
        std::span<glm::vec4> col = geo::colors::add(*m, set);
        for (size_t f = 0; f < nF; ++f)
            if (faceOf[f] != geo::kInvalid)
                for (int32_t k = offsets[f]; k < offsets[f + 1]; ++k)
                    if (cornerOf[size_t(k)] != geo::kInvalid)
                        col[cornerOf[size_t(k)]] = colorOf(a, a.domain == 0 ? size_t(value<int32_t>(*cornerVert, size_t(k))) : size_t(k));
        if (a.name == defaultColor)
            firstSet = set;
    }
    if (!firstSet.empty())
        geo::colors::move(*m, firstSet, 0);

    // Custom normals: Blender 4.4's free normals are plain vectors; older encoded ones are not read.
    if (const MeshAttr* cn = [&]() -> const MeshAttr* {
            for (int d : {3, 0, 2})
                if (const MeshAttr* a = find("custom_normal", d, 7))
                    return a;
            return nullptr;
        }())
    {
        std::span<glm::vec3> nrm = m->cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
        for (size_t f = 0; f < nF; ++f)
            for (int32_t k = offsets[f]; faceOf[f] != geo::kInvalid && k < offsets[f + 1]; ++k)
            {
                const size_t src = cn->domain == 3 ? size_t(k) : cn->domain == 0 ? size_t(value<int32_t>(*cornerVert, size_t(k))) : f;
                const glm::dvec3 n = axes.direction(glm::dvec3(value<glm::vec3>(*cn, src)));
                const double len = glm::length(n);
                if (cornerOf[size_t(k)] != geo::kInvalid)
                    nrm[cornerOf[size_t(k)]] = glm::vec3(len > 0.0 ? n / len : glm::dvec3(0.0));
            }
    }
    else
        for (const View& layer : me.sub("ldata").array("layers", size_t(std::max(0, me.sub("ldata").i("totlayer"))), "CustomDataLayer"))
            if (layer.i("type") == 41)
                warnOnce("customNormals:" + meshName, std::format("mesh '{}': custom split normals from before Blender 4.4 are not read; normals are recomputed", meshName));

    // Everything else that maps onto a Lila attribute type.
    static constexpr geo::Domain kDomains[] = {geo::Domain::Point, geo::Domain::Edge, geo::Domain::Face, geo::Domain::Corner};
    for (const MeshAttr& a : attrs)
    {
        if (a.domain < 0 || a.domain > 3 || isInternal(a.name) || handled.count(a.name) || a.name.starts_with("crease_") || a.name.starts_with("bevel_weight_"))
            continue;
        const size_t count = a.domain == 0 ? nV : a.domain == 1 ? nE : a.domain == 2 ? nF : nL;
        if (!a.covers(count))
            continue;
        geo::AttributeSet& set = m->attributes(kDomains[a.domain]);
        auto lilaIndex = [&](size_t i) -> Index {
            switch (a.domain)
            {
            case 0: return Index(i);
            case 1: return edgeOf[i];
            case 2: return faceOf[i];
            default: return cornerOf[i];
            }
        };
        auto copy = [&]<class T, class S>(std::span<T> dst, auto convert) {
            for (size_t i = 0; i < count; ++i)
                if (const Index li = lilaIndex(i); li != geo::kInvalid && li < dst.size())
                    dst[li] = convert(value<S>(a, i));
        };
        switch (a.type)
        {
        case 0: copy.template operator()<bool, uint8_t>(set.getOrAdd<bool>(a.name), [](uint8_t v) { return v != 0; }); break;
        case 1: copy.template operator()<int32_t, int8_t>(set.getOrAdd<int32_t>(a.name), [](int8_t v) { return int32_t(v); }); break;
        case 3: copy.template operator()<int32_t, int32_t>(set.getOrAdd<int32_t>(a.name), [](int32_t v) { return v; }); break;
        case 5: copy.template operator()<float, float>(set.getOrAdd<float>(a.name), [](float v) { return v; }); break;
        case 6: copy.template operator()<glm::vec2, glm::vec2>(set.getOrAdd<glm::vec2>(a.name), [](glm::vec2 v) { return v; }); break;
        case 7: copy.template operator()<glm::vec3, glm::vec3>(set.getOrAdd<glm::vec3>(a.name), [](glm::vec3 v) { return v; }); break;
        case 10:
        case 11: copy.template operator()<glm::vec4, glm::vec4>(set.getOrAdd<glm::vec4>(a.name), [](glm::vec4 v) { return v; }); break;
        default: break;
        }
    }
    return m;
}

void Importer::smoothByAngle(const View& object, const View& me, geo::Mesh& m)
{
    double angle = -1.0;
    bool keepSharp = true;
    for (const View& mod : object.list("modifiers"))
    {
        const View head = mod.has("modifier") ? mod.sub("modifier") : mod;
        if (head.i("type") != md::kNodes || !(head.i("mode") & 1))
            continue;
        const View group = mod.deref("node_group");
        if (!group || !group.idName().starts_with("Smooth by Angle"))
            continue;
        angle = glm::radians(30.0);
        // The modifier's inputs: Input_1 / Socket_1 the angle, Socket_2 / Input_2 "Ignore Sharpness".
        if (const View props = mod.sub("settings").deref("properties"))
            for (const View& p : props.sub("data").list("group", "IDProperty"))
            {
                const std::string name = p.str("name");
                const View d = p.sub("data");
                const int type = p.i("type");
                if (type == 2 && (name == "Input_1" || name == "Socket_1"))
                {
                    const int32_t bits = d.i("val");
                    float f;
                    std::memcpy(&f, &bits, 4);
                    angle = f;
                }
                else if ((type == 1 || type == 13) && (name == "Socket_2" || name == "Input_2"))
                    keepSharp = d.i("val") == 0;
            }
    }
    // Before 4.1: auto smooth on the mesh.
    if (angle < 0.0 && (me.i("flag") & (1 << 5)) && me.has("smoothresh"))
        angle = me.num("smoothresh");
    if (angle < 0.0)
        return;
    std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
    std::vector<glm::dvec3> normals(m.faceCapacity());
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            normals[f] = faceNormal(m, f);
    const double limit = std::cos(angle);
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        if (!keepSharp)
            hard[e] = false;
        const geo::Index c = m.edgeCorner(e);
        if (c == geo::kInvalid)
            continue;
        const geo::Index other = m.cornerRadial(c);
        if (other == c || other == geo::kInvalid)
            continue;
        if (glm::dot(normals[m.cornerFace(c)], normals[m.cornerFace(other)]) < limit)
            hard[e] = true;
    }
}
} // namespace lila::blend
