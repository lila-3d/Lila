#include "scene/MeshData.h"

#include "core/PackedArray.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <stdexcept>

namespace lila::scene
{
namespace
{
using geo::Index;

json layerData(const geo::Attribute& a, std::span<const Index> order)
{
    json data = json::array();
    int n = geo::attrTypeComponents(a.type());
    for (Index i : order)
    {
        const std::byte* p = a.raw(i);
        switch (a.type())
        {
        case geo::AttrType::Int:
        {
            int32_t v;
            std::memcpy(&v, p, 4);
            data.push_back(v);
            break;
        }
        case geo::AttrType::Bool: data.push_back(bool(*reinterpret_cast<const bool*>(p))); break;
        default:
            for (int k = 0; k < n; ++k)
            {
                float v;
                std::memcpy(&v, p + k * 4, 4);
                data.push_back(v);
            }
        }
    }
    return data;
}

/** A flat numeric array from a mesh document: plain json, or packed as a binary scene file loads it. */
struct Numbers
{
    const json& j;
    const uint8_t* data = nullptr;
    packed::DType dtype = packed::DType::F64;
    size_t n = 0;

    explicit Numbers(const json& v) : j(v)
    {
        if (packed::is(v))
        {
            data = v.get_binary().data();
            dtype = packed::dtype(v);
            n = packed::count(v);
        }
        else
            n = v.is_array() ? v.size() : 0;
    }
    size_t size() const { return n; }
    double number(size_t i) const
    {
        if (!data)
            return j[i].is_boolean() ? double(j[i].get<bool>()) : j[i].get<double>();
        switch (dtype)
        {
        case packed::DType::F64: { double v; std::memcpy(&v, data + i * 8, 8); return v; }
        case packed::DType::F32: { float v; std::memcpy(&v, data + i * 4, 4); return v; }
        case packed::DType::I32: { int32_t v; std::memcpy(&v, data + i * 4, 4); return v; }
        case packed::DType::U32: { uint32_t v; std::memcpy(&v, data + i * 4, 4); return v; }
        case packed::DType::Bool: return data[i];
        }
        return 0.0;
    }
    std::vector<Index> indices() const
    {
        std::vector<Index> out(n);
        if (data && (dtype == packed::DType::U32 || dtype == packed::DType::I32))
            std::memcpy(out.data(), data, n * sizeof(Index));
        else
            for (size_t i = 0; i < n; ++i)
                out[i] = Index(number(i));
        return out;
    }
};

void setLayerData(geo::Attribute& a, const json& data, std::span<const Index> order)
{
    const Numbers values(data);
    const int n = geo::attrTypeComponents(a.type());
    // Every entry, dropped or not, takes the same number of values, so each one's offset is known up front.
    const size_t stride = a.type() == geo::AttrType::Int || a.type() == geo::AttrType::Bool ? 1 : size_t(n);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, order.size(), 16384), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t k = r.begin(); k != r.end(); ++k)
        {
            const Index i = order[k];
            if (i == geo::kInvalid)
                continue;
            const size_t at = k * stride;
            std::byte* p = a.raw(i);
            switch (a.type())
            {
            case geo::AttrType::Int:
            {
                int32_t v = at < values.size() ? int32_t(values.number(at)) : 0;
                std::memcpy(p, &v, 4);
                break;
            }
            case geo::AttrType::Bool: *reinterpret_cast<bool*>(p) = at < values.size() && values.number(at) != 0.0; break;
            default:
                for (int c = 0; c < n; ++c)
                {
                    float v = at + c < values.size() ? float(values.number(at + c)) : 0.0f;
                    std::memcpy(p + c * 4, &v, 4);
                }
            }
        }
    });
}

geo::AttrType typeFromName(const std::string& s)
{
    for (auto t : {geo::AttrType::Float, geo::AttrType::Float2, geo::AttrType::Float3, geo::AttrType::Float4, geo::AttrType::Int,
                   geo::AttrType::Bool})
        if (s == geo::attrTypeName(t))
            return t;
    throw std::invalid_argument("unknown attribute type '" + s + "'");
}
} // namespace

json meshToJson(const geo::Mesh& mesh)
{
    LILA_ZONE();

    json points = json::array();
    std::vector<Index> pointOrder, pointRemap(mesh.pointCapacity(), geo::kInvalid);
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
        {
            pointRemap[p] = Index(pointOrder.size());
            pointOrder.push_back(p);
            const glm::dvec3& v = mesh.position(p);
            points.push_back({v.x, v.y, v.z});
        }

    json sizes = json::array(), indices = json::array();
    std::vector<Index> faceOrder, cornerOrder;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        faceOrder.push_back(f);
        sizes.push_back(mesh.faceSize(f));
        mesh.forEachFaceCorner(f, [&](Index c) {
            cornerOrder.push_back(c);
            indices.push_back(pointRemap[mesh.cornerPoint(c)]);
        });
    }

    std::vector<Index> edgeOrder;
    json edges = json::array();
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
        if (mesh.edgeValid(e))
        {
            edgeOrder.push_back(e);
            edges.push_back(pointRemap[mesh.edgePoint(e, 0)]);
            edges.push_back(pointRemap[mesh.edgePoint(e, 1)]);
        }

    json layers = json::array();
    const std::pair<geo::Domain, const std::vector<Index>*> domains[] = {
        {geo::Domain::Point, &pointOrder}, {geo::Domain::Edge, &edgeOrder}, {geo::Domain::Face, &faceOrder}, {geo::Domain::Corner, &cornerOrder}};
    for (auto [domain, order] : domains)
    {
        const geo::AttributeSet& set = mesh.attributes(domain);
        for (size_t i = 0; i < set.layerCount(); ++i)
        {
            const geo::Attribute& a = set.layer(i);
            layers.push_back({{"name", a.name()}, {"domain", geo::domainName(domain)}, {"type", geo::attrTypeName(a.type())},
                              {"data", layerData(a, *order)}});
        }
    }

    json out = {{"points", std::move(points)}, {"faceSizes", std::move(sizes)}, {"faceIndices", std::move(indices)}};
    if (!edges.empty())
        out["edges"] = std::move(edges);
    if (!layers.empty())
        out["layers"] = std::move(layers);
    if (!mesh.materialNames().empty())
        out["materials"] = mesh.materialNames();
    if (const geo::ColorSetInfo& cs = mesh.colorSetInfo(); !cs.current.empty() || !cs.half.empty())
        out["colorSets"] = {{"current", cs.current}, {"half", cs.half}};
    return out;
}

namespace
{
/** Runs fn(begin, end) over [0, n) in parallel blocks; small ranges stay on this thread. */
template <class F> void blocks(size_t n, F&& fn)
{
    constexpr size_t kGrain = 1 << 14;
    if (n <= kGrain)
        return n ? fn(size_t(0), n) : void();
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, kGrain), [&](const tbb::blocked_range<size_t>& r) { fn(r.begin(), r.end()); });
}

/** Layer values in the order given, packed as-is: Int and float components are 4 bytes each, Bool one. */
json packedLayer(const geo::Attribute& a, const std::vector<Index>& order)
{
    if (a.type() == geo::AttrType::Bool)
    {
        bool* out;
        json data = packed::alloc(order.size(), out);
        blocks(order.size(), [&](size_t b, size_t e) {
            for (size_t k = b; k < e; ++k)
                out[k] = *reinterpret_cast<const bool*>(a.raw(order[k]));
        });
        return data;
    }
    const size_t n = size_t(geo::attrTypeComponents(a.type()));
    auto gather = [&](auto* out) {
        blocks(order.size(), [&](size_t b, size_t e) {
            for (size_t k = b; k < e; ++k)
                std::memcpy(out + k * n, a.raw(order[k]), n * 4);
        });
    };
    if (a.type() == geo::AttrType::Int)
    {
        int32_t* out;
        json data = packed::alloc(order.size() * n, out);
        gather(out);
        return data;
    }
    float* out;
    json data = packed::alloc(order.size() * n, out);
    gather(out);
    return data;
}
} // namespace

json meshToJsonPacked(const geo::Mesh& mesh)
{
    LILA_ZONE();

    std::vector<Index> pointOrder, pointRemap(mesh.pointCapacity(), geo::kInvalid);
    pointOrder.reserve(mesh.numPoints());
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
        {
            pointRemap[p] = Index(pointOrder.size());
            pointOrder.push_back(p);
        }

    // Face sizes first; their running sum says where each face's corners go, so the corners fill in parallel.
    std::vector<Index> faceOrder, sizes, cornerStart;
    faceOrder.reserve(mesh.numFaces());
    sizes.reserve(mesh.numFaces());
    cornerStart.reserve(mesh.numFaces());
    size_t corners = 0;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
        {
            faceOrder.push_back(f);
            sizes.push_back(mesh.faceSize(f));
            cornerStart.push_back(Index(corners));
            corners += mesh.faceSize(f);
        }
    std::vector<Index> cornerOrder(corners);
    Index* indices;
    json faceIndices = packed::alloc(corners, indices);

    std::vector<Index> edgeOrder;
    edgeOrder.reserve(mesh.numEdges());
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
        if (mesh.edgeValid(e))
            edgeOrder.push_back(e);

    double* points;
    json pointsJson = packed::alloc(pointOrder.size() * 3, points, 3);
    Index* edges;
    json edgesJson = packed::alloc(edgeOrder.size() * 2, edges);
    blocks(pointOrder.size(), [&](size_t b, size_t e) {
        for (size_t k = b; k < e; ++k)
            std::memcpy(points + k * 3, &mesh.position(pointOrder[k]), sizeof(glm::dvec3));
    });
    blocks(faceOrder.size(), [&](size_t b, size_t e) {
        for (size_t k = b; k < e; ++k)
        {
            Index at = cornerStart[k];
            const Index end = at + sizes[k];
            mesh.forEachFaceCorner(faceOrder[k], [&](Index c) {
                if (at == end)
                    return;
                cornerOrder[at] = c;
                indices[at++] = pointRemap[mesh.cornerPoint(c)];
            });
        }
    });
    blocks(edgeOrder.size(), [&](size_t b, size_t e) {
        for (size_t k = b; k < e; ++k)
        {
            edges[k * 2] = pointRemap[mesh.edgePoint(edgeOrder[k], 0)];
            edges[k * 2 + 1] = pointRemap[mesh.edgePoint(edgeOrder[k], 1)];
        }
    });

    std::vector<std::pair<const geo::Attribute*, geo::Domain>> layerList;
    std::vector<const std::vector<Index>*> layerOrder;
    const std::pair<geo::Domain, const std::vector<Index>*> domains[] = {
        {geo::Domain::Point, &pointOrder}, {geo::Domain::Edge, &edgeOrder}, {geo::Domain::Face, &faceOrder}, {geo::Domain::Corner, &cornerOrder}};
    for (auto [domain, order] : domains)
    {
        const geo::AttributeSet& set = mesh.attributes(domain);
        for (size_t i = 0; i < set.layerCount(); ++i)
        {
            layerList.push_back({&set.layer(i), domain});
            layerOrder.push_back(order);
        }
    }
    std::vector<json> layerValues(layerList.size());
    tbb::parallel_for(size_t(0), layerList.size(), [&](size_t i) { layerValues[i] = packedLayer(*layerList[i].first, *layerOrder[i]); });
    json layers = json::array();
    for (size_t i = 0; i < layerList.size(); ++i)
    {
        const geo::Attribute& a = *layerList[i].first;
        layers.push_back({{"name", a.name()}, {"domain", geo::domainName(layerList[i].second)}, {"type", geo::attrTypeName(a.type())},
                          {"data", std::move(layerValues[i])}});
    }

    json out = {{"points", std::move(pointsJson)}, {"faceSizes", packed::make(std::span<const Index>(sizes))},
                {"faceIndices", std::move(faceIndices)}};
    if (!edgeOrder.empty())
        out["edges"] = std::move(edgesJson);
    if (!layers.empty())
        out["layers"] = std::move(layers);
    if (!mesh.materialNames().empty())
        out["materials"] = mesh.materialNames();
    if (const geo::ColorSetInfo& cs = mesh.colorSetInfo(); !cs.current.empty() || !cs.half.empty())
        out["colorSets"] = {{"current", cs.current}, {"half", cs.half}};
    return out;
}

MeshPtr meshFromJson(const json& j)
{
    LILA_ZONE();

    static const json kNone = json::array();
    auto member = [&](const char* key) -> const json& {
        auto it = j.find(key);
        return it == j.end() ? kNone : *it;
    };
    std::vector<glm::dvec3> points;
    if (const json& pj = member("points"); packed::is(pj))
    {
        const Numbers flat(pj);
        points.resize(flat.size() / 3);
        tbb::parallel_for(size_t(0), points.size(), [&](size_t i) {
            points[i] = {flat.number(i * 3), flat.number(i * 3 + 1), flat.number(i * 3 + 2)};
        });
    }
    else
        for (const json& p : pj)
            points.emplace_back(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
    std::vector<Index> sizes = Numbers(member("faceSizes")).indices();
    std::vector<Index> indices = Numbers(member("faceIndices")).indices();
    auto mesh = std::make_shared<geo::Mesh>(geo::Mesh::fromPolygons(points, sizes, indices));

    std::vector<Index> pointOrder(mesh->pointCapacity());
    for (Index p = 0; p < mesh->pointCapacity(); ++p)
        pointOrder[p] = p;

    // Faces that fromPolygons rejected leave holes in the order; match by walking the input.
    std::vector<Index> faceOrder, cornerOrder;
    {
        Index f = 0, at = 0;
        for (Index s : sizes)
        {
            std::span<const Index> pts(indices.data() + at, s);
            at += s;
            Index found = geo::kInvalid;
            if (f < mesh->faceCapacity() && mesh->faceValid(f) && mesh->faceSize(f) == s && mesh->cornerPoint(mesh->faceCorner(f)) == pts[0])
                found = f++;
            faceOrder.push_back(found);
            if (found == geo::kInvalid)
                cornerOrder.insert(cornerOrder.end(), s, geo::kInvalid);
            else
                mesh->forEachFaceCorner(found, [&](Index c) { cornerOrder.push_back(c); });
        }
    }

    const std::vector<Index> edges = Numbers(member("edges")).indices();
    std::vector<Index> edgeOrder(edges.size() / 2);
    tbb::parallel_for(size_t(0), edgeOrder.size(), [&](size_t i) { edgeOrder[i] = mesh->findEdge(edges[i * 2], edges[i * 2 + 1]); });

    for (const json& layer : member("layers"))
    {
        std::string domainName = layer.value("domain", "");
        geo::Domain domain = geo::Domain::Point;
        const std::vector<Index>* order = &pointOrder;
        if (domainName == "edge")
            domain = geo::Domain::Edge, order = &edgeOrder;
        else if (domainName == "face")
            domain = geo::Domain::Face, order = &faceOrder;
        else if (domainName == "corner")
            domain = geo::Domain::Corner, order = &cornerOrder;
        geo::Attribute& a = mesh->attributes(domain).add(layer.value("name", ""), typeFromName(layer.value("type", "")));
        setLayerData(a, layer["data"], *order);
    }
    if (j.contains("materials"))
        mesh->materialNames() = j["materials"].get<std::vector<std::string>>();
    if (auto cs = j.find("colorSets"); cs != j.end() && cs->is_object())
    {
        mesh->colorSetInfo().current = cs->value("current", "");
        mesh->colorSetInfo().half = cs->value("half", std::vector<std::string>{});
    }
    geo::colors::adopt(*mesh);
    return mesh;
}
} // namespace lila::scene
