#include "collada/Collada.h"

#include "collada/ColladaInternal.h"
#include "collada/Xml.h"
#include "core/Profile.h"
#include "fbx/ImportCommon.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Rig.h"
#include "scene/RotationCurve.h"
#include "scene/Skin.h"
#include "scene/Take.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <functional>
#include <unordered_map>

namespace lila::collada
{
namespace
{
using namespace scene;
using geo::Index;
using geo::kInvalid;
using xml::Element;

constexpr double kEps = 1e-9;

std::string_view stripHash(std::string_view s) { return !s.empty() && s[0] == '#' ? s.substr(1) : s; }

/** Column-major glm from COLLADA's row-major 16 values. */
glm::dmat4 matrixFrom(const double* v)
{
    glm::dmat4 m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m[c][r] = v[r * 4 + c];
    return m;
}

bool nearIdentity(const glm::dmat4& m)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(m[c][r] - (c == r ? 1.0 : 0.0)) > 1e-9)
                return false;
    return true;
}

glm::dvec3 clean(glm::dvec3 v)
{
    for (int i = 0; i < 3; ++i)
        if (std::abs(v[i]) < 1e-9)
            v[i] = 0.0;
    return v;
}

/** A <source>: its numbers or names with the accessor's stride and offset applied. */
struct Source
{
    std::vector<double> values;
    std::vector<std::string> names;
    size_t stride = 1;
    size_t count = 0;

    double at(size_t i, size_t comp) const
    {
        const size_t k = i * stride + comp;
        return k < values.size() ? values[k] : 0.0;
    }
};

// One transform stack element, in the order the node lists them.
struct StackItem
{
    enum Kind
    {
        Matrix,
        Translate,
        Rotate,
        Scale,
        LookAt,
        Skew,
    } kind;
    std::string sid;
    std::vector<double> v;
};

glm::dmat4 itemMatrix(const StackItem& s)
{
    const auto& v = s.v;
    auto at = [&](size_t i) { return i < v.size() ? v[i] : 0.0; };
    switch (s.kind)
    {
    case StackItem::Matrix: return v.size() >= 16 ? matrixFrom(v.data()) : glm::dmat4(1.0);
    case StackItem::Translate: return glm::translate(glm::dmat4(1.0), glm::dvec3(at(0), at(1), at(2)));
    case StackItem::Rotate:
    {
        glm::dvec3 axis(at(0), at(1), at(2));
        if (glm::length(axis) < kEps)
            return glm::dmat4(1.0);
        return glm::rotate(glm::dmat4(1.0), glm::radians(at(3)), glm::normalize(axis));
    }
    case StackItem::Scale: return glm::scale(glm::dmat4(1.0), glm::dvec3(v.size() > 0 ? at(0) : 1.0, v.size() > 1 ? at(1) : 1.0, v.size() > 2 ? at(2) : 1.0));
    case StackItem::LookAt:
    {
        const glm::dvec3 eye(at(0), at(1), at(2)), target(at(3), at(4), at(5)), up(at(6), at(7), at(8));
        return glm::inverse(glm::lookAt(eye, target, up));
    }
    case StackItem::Skew:
    {
        // COLLADA skew: angle in degrees, rotation axis, translation axis (RenderMan's convention).
        const double angle = glm::radians(at(0));
        glm::dvec3 a(at(1), at(2), at(3)), t(at(4), at(5), at(6));
        if (glm::length(a) < kEps || glm::length(t) < kEps)
            return glm::dmat4(1.0);
        a = glm::normalize(a);
        t = glm::normalize(t);
        const double shear = std::tan(angle);
        glm::dmat4 m(1.0);
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                m[c][r] += shear * t[r] * a[c];
        return m;
    }
    }
    return glm::dmat4(1.0);
}

/** One animation channel resolved to its stack element. */
struct Channel
{
    size_t item = 0;
    int component = -1; // -1 the whole element; for matrices the row-major index
    std::vector<double> times;
    std::vector<double> values; // stride per key
    size_t stride = 1;
    std::vector<std::string> interp;
    std::vector<double> inTan, outTan; // 2 per component per key (time, value)
    size_t tanStride = 0;

    /** Value (stride numbers) at time t. */
    void evaluate(double t, double* out) const
    {
        const size_t n = times.size();
        if (n == 0)
            return;
        auto key = [&](size_t k, size_t c) { return values[k * stride + c]; };
        if (t <= times.front() || n == 1)
        {
            for (size_t c = 0; c < stride; ++c)
                out[c] = key(0, c);
            return;
        }
        if (t >= times.back())
        {
            for (size_t c = 0; c < stride; ++c)
                out[c] = key(n - 1, c);
            return;
        }
        const size_t b = size_t(std::upper_bound(times.begin(), times.end(), t) - times.begin());
        const size_t a = b - 1;
        const double t0 = times[a], t1 = times[b];
        const double u = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0;
        const std::string& mode = a < interp.size() ? interp[a] : std::string();
        for (size_t c = 0; c < stride; ++c)
        {
            const double v0 = key(a, c), v1 = key(b, c);
            if (mode == "STEP")
                out[c] = v0;
            else if (mode == "BEZIER" && tanStride == stride * 2 && outTan.size() >= (a + 1) * tanStride && inTan.size() >= (b + 1) * tanStride)
            {
                const double cx0 = outTan[a * tanStride + c * 2], cy0 = outTan[a * tanStride + c * 2 + 1];
                const double cx1 = inTan[b * tanStride + c * 2], cy1 = inTan[b * tanStride + c * 2 + 1];
                auto bez = [](double p0, double p1, double p2, double p3, double s) {
                    const double r = 1.0 - s;
                    return r * r * r * p0 + 3 * r * r * s * p1 + 3 * r * s * s * p2 + s * s * s * p3;
                };
                // Solve x(s) = t by bisection; x is monotonic for sane tangents.
                double lo = 0.0, hi = 1.0, s = u;
                for (int it = 0; it < 40; ++it)
                {
                    s = 0.5 * (lo + hi);
                    (bez(t0, cx0, cx1, t1, s) < t ? lo : hi) = s;
                }
                out[c] = bez(v0, cy0, cy1, v1, s);
            }
            else
                out[c] = v0 + (v1 - v0) * u;
        }
    }
};

struct FileNode
{
    const Element* e = nullptr;
    std::vector<StackItem> stack;
    std::vector<Channel> channels;
    bool joint = false;
    NodeId id = kNoNode;
    glm::dmat3 pre{1.0};
    RotateOrder order = RotateOrder::XYZ;
    fbx::Trs rest;

    glm::dmat4 localAt(double t) const
    {
        std::vector<StackItem> s = stack;
        for (const Channel& ch : channels)
        {
            StackItem& it = s[ch.item];
            std::vector<double> v(ch.stride);
            ch.evaluate(t, v.data());
            if (ch.component < 0)
            {
                if (v.size() >= it.v.size())
                    it.v.assign(v.begin(), v.begin() + std::ptrdiff_t(std::max(it.v.size(), v.size() == 16 ? size_t(16) : it.v.size())));
                else
                    std::copy(v.begin(), v.end(), it.v.begin());
            }
            else if (size_t(ch.component) < it.v.size() && !v.empty())
                it.v[size_t(ch.component)] = v[0];
        }
        glm::dmat4 m(1.0);
        for (const StackItem& i : s)
            m = m * itemMatrix(i);
        return m;
    }
};

/**
 * The stack as Maya writes it unbaked: translate, then jointOrientZ/Y/X, then rotateX/Y/Z in some order, then
 * scale, around pivots that are left out. Found only when every rotate has one of those sids.
 */
struct MayaElements
{
    bool found = false;
    glm::dmat3 jointOrient{1.0}; // file axes
    RotateOrder order = RotateOrder::XYZ;
    glm::dvec3 rotate{0.0}; // the file's rest angles, a reference for continuity
};

MayaElements mayaElements(const std::vector<StackItem>& stack)
{
    MayaElements out;
    std::string listed; // rotate axes as listed
    for (const StackItem& it : stack)
    {
        if (it.kind != StackItem::Rotate)
            continue;
        if (it.sid.size() == 12 && it.sid.starts_with("jointOrient"))
            out.jointOrient = out.jointOrient * glm::dmat3(itemMatrix(it));
        else if (it.sid.size() == 7 && it.sid.starts_with("rotate") && it.sid[6] >= 'X' && it.sid[6] <= 'Z')
        {
            listed += it.sid[6];
            if (it.v.size() >= 4)
                out.rotate[it.sid[6] - 'X'] = it.v[3];
        }
        else
            return {};
    }
    if (listed.size() != 3)
        return {};
    // Listed last-applied first; RotateOrder names the first-applied axis first.
    static const std::pair<const char*, RotateOrder> kOrders[] = {{"ZYX", RotateOrder::XYZ}, {"XZY", RotateOrder::YZX}, {"YXZ", RotateOrder::ZXY},
                                                                  {"YZX", RotateOrder::XZY}, {"ZXY", RotateOrder::YXZ}, {"XYZ", RotateOrder::ZYX}};
    for (const auto& [l, o] : kOrders)
        if (listed == l)
        {
            out.order = o;
            out.found = true;
        }
    return out;
}

struct BuiltMesh
{
    MeshPtr mesh;
    /** Per point: the position index in the file, for skin weights. */
    std::vector<int64_t> source;
};

struct Importer
{
    Graph& graph;
    const Element& root;
    fs::path dir;
    const ImportOptions& opts;
    ImportResult& r;
    glm::dmat3 F{1.0};
    glm::dmat4 C{1.0}, Cinv{1.0};
    std::unordered_map<std::string, const Element*> byId;
    std::map<const Element*, FileNode> nodes;
    std::map<std::string, std::shared_ptr<BuiltMesh>> geometries;
    std::map<std::string, std::string> materialName; // material id -> Lila material name
    struct SkinUse
    {
        NodeId mesh;
        const Element* controller;
        const Element* instance;
        std::shared_ptr<BuiltMesh> built;
    };
    std::vector<SkinUse> skins;

    glm::dmat4 toLila(const glm::dmat4& m) const { return C * m * Cinv; }

    const Element* lookup(std::string_view url) const
    {
        auto it = byId.find(std::string(stripHash(url)));
        return it == byId.end() ? nullptr : it->second;
    }

    Source source(const Element* src) const;
    std::shared_ptr<BuiltMesh> buildGeometry(const Element* geometry);
    void importMaterials();
    std::string imagePath(const Element* effect, std::string_view textureRef) const;
    std::vector<const Element*> skinJoints(const Element* controller, const Element* instance);
    void markJoints(const Element* visualScene);
    void create(const Element* e, NodeId parent, const Element* via);
    NodeId attachMesh(NodeId node, const std::shared_ptr<BuiltMesh>& built, const Element* instance, bool own, const std::string& name);
    void buildSkins();
    void readAnimations();
    void importAnimation();
};

Source Importer::source(const Element* src) const
{
    Source s;
    if (!src)
        return s;
    if (const Element* fa = src->child("float_array"))
        s.values = xml::numbers(fa->text);
    else if (const Element* na = src->child("Name_array"))
        s.names = xml::words(na->text);
    else if (const Element* ia = src->child("IDREF_array"))
        s.names = xml::words(ia->text);
    else if (const Element* ints = src->child("int_array"))
        s.values = xml::numbers(ints->text);
    const Element* acc = src->child("technique_common") ? src->child("technique_common")->child("accessor") : nullptr;
    if (!acc)
    {
        s.count = s.values.empty() ? s.names.size() : s.values.size();
        return s;
    }
    const size_t stride = std::max<size_t>(1, size_t(std::atoll(std::string(acc->attr("stride")).c_str())));
    const size_t offset = size_t(std::atoll(std::string(acc->attr("offset")).c_str()));
    const size_t count = size_t(std::atoll(std::string(acc->attr("count")).c_str()));
    // Params without a name are skipped by the accessor; keep the named ones only.
    std::vector<size_t> used;
    size_t p = 0;
    for (const Element* param : acc->all("param"))
    {
        if (!param->attr("name").empty())
            used.push_back(p);
        p += param->attr("type") == "float4x4" ? 16 : 1;
    }
    if (used.empty() || p > stride)
        for (size_t k = 0; k < stride; ++k)
            used.push_back(k);
    const bool whole = used.size() == stride && offset == 0;
    if (!s.values.empty())
    {
        if (!whole || p == 16)
        {
            std::vector<double> out;
            const size_t width = p == 16 ? 16 : used.size();
            for (size_t i = 0; i < count; ++i)
                for (size_t k = 0; k < width; ++k)
                {
                    const size_t at = offset + i * stride + (p == 16 ? k : used[k]);
                    out.push_back(at < s.values.size() ? s.values[at] : 0.0);
                }
            s.values = std::move(out);
            s.stride = width;
        }
        else
            s.stride = stride;
    }
    else
        s.stride = 1;
    s.count = count;
    return s;
}

// Geometry -----------------------------------------------------------------------------------------------------------

std::shared_ptr<BuiltMesh> Importer::buildGeometry(const Element* geometry)
{
    const std::string gid(geometry->attr("id"));
    if (auto it = geometries.find(gid); it != geometries.end())
        return it->second;
    auto out = std::make_shared<BuiltMesh>();
    geometries[gid] = out;
    const Element* mesh = geometry->child("mesh");
    if (!mesh)
    {
        const Element* other = geometry->children.empty() ? nullptr : geometry->children[0].get();
        r.skipped.push_back(std::format("geometry '{}' ({})", gid, other ? other->name : "empty"));
        return out;
    }
    std::unordered_map<std::string, Source> sources;
    auto sourceOf = [&](std::string_view url) -> const Source& {
        const std::string id(stripHash(url));
        auto it = sources.find(id);
        if (it == sources.end())
            it = sources.emplace(id, source(lookup(id))).first;
        return it->second;
    };
    const Element* vertices = mesh->child("vertices");
    const std::string verticesId = vertices ? std::string(vertices->attr("id")) : std::string();
    const Source* positions = nullptr;
    // Inputs on <vertices> are per position index.
    struct Input
    {
        std::string semantic;
        const Source* src = nullptr;
        size_t offset = 0;
        int set = 0;
        bool perVertex = false;
    };
    std::vector<Input> vertexInputs;
    if (vertices)
        for (const Element* in : vertices->all("input"))
        {
            const Source& s = sourceOf(in->attr("source"));
            if (in->attr("semantic") == "POSITION")
                positions = &s;
            else
                vertexInputs.push_back({std::string(in->attr("semantic")), &s, 0, 0, true});
        }
    if (!positions || positions->values.empty())
    {
        r.warnings.push_back(std::format("geometry '{}' has no positions", gid));
        return out;
    }

    auto m = std::make_shared<geo::Mesh>();
    const size_t pointCount = positions->values.size() / positions->stride;
    for (size_t p = 0; p < pointCount; ++p)
        m->addPoint(glm::dvec3(C * glm::dvec4(positions->at(p, 0), positions->at(p, 1), positions->at(p, 2), 1.0)));
    for (size_t p = 0; p < pointCount; ++p)
        out->source.push_back(int64_t(p));

    struct Corner
    {
        glm::vec3 normal{0.0f};
        bool hasNormal = false;
        std::vector<glm::vec2> uv;
        std::vector<glm::vec4> color;
    };
    std::vector<Corner> corners; // per created face corner, in face order
    std::vector<Index> faceOrder;
    std::vector<int32_t> faceSlot;
    std::vector<std::string>& slotNames = m->materialNames();
    std::map<std::string, int32_t> slotOf;
    size_t uvSets = 0, colorSets = 0, degenerate = 0;
    bool anyNormals = false;
    const glm::dmat3 nm = glm::inverse(glm::transpose(F));

    for (const auto& prim : mesh->children)
    {
        const std::string& tag = prim->name;
        const bool tris = tag == "triangles", list = tag == "polylist", polys = tag == "polygons";
        if (!tris && !list && !polys && tag != "trifans" && tag != "tristrips")
        {
            if (tag == "lines" || tag == "linestrips")
                r.skipped.push_back(std::format("geometry '{}': {}", gid, tag));
            continue;
        }
        if (tag == "trifans" || tag == "tristrips")
        {
            r.skipped.push_back(std::format("geometry '{}': {} (not supported)", gid, tag));
            continue;
        }
        std::vector<Input> inputs;
        size_t stride = 0;
        for (const Element* in : prim->all("input"))
        {
            const size_t offset = size_t(std::atoll(std::string(in->attr("offset")).c_str()));
            stride = std::max(stride, offset + 1);
            const std::string semantic(in->attr("semantic"));
            const int set = in->has("set") ? std::atoi(std::string(in->attr("set")).c_str()) : -1;
            if (semantic == "VERTEX" || stripHash(in->attr("source")) == verticesId)
                inputs.push_back({"VERTEX", nullptr, offset, 0, false});
            else
                inputs.push_back({semantic, &sourceOf(in->attr("source")), offset, set, false});
        }
        size_t vertexOffset = SIZE_MAX;
        for (const Input& in : inputs)
            if (in.semantic == "VERTEX")
                vertexOffset = in.offset;
        if (vertexOffset == SIZE_MAX || stride == 0)
            continue;
        // Texcoord and color sets in the order they first appear, by set number.
        std::vector<const Input*> uvIn, colIn;
        const Input* nrmIn = nullptr;
        std::vector<Input> all = inputs;
        for (Input vi : vertexInputs)
        {
            vi.offset = vertexOffset;
            all.push_back(vi);
        }
        for (const Input& in : all)
            if (in.semantic == "NORMAL" && !nrmIn)
                nrmIn = &in;
            else if (in.semantic == "TEXCOORD")
                uvIn.push_back(&in);
            else if (in.semantic == "COLOR")
                colIn.push_back(&in);
        std::stable_sort(uvIn.begin(), uvIn.end(), [](const Input* a, const Input* b) { return a->set < b->set; });
        std::stable_sort(colIn.begin(), colIn.end(), [](const Input* a, const Input* b) { return a->set < b->set; });
        uvSets = std::max(uvSets, uvIn.size());
        colorSets = std::max(colorSets, colIn.size());
        anyNormals = anyNormals || nrmIn;

        const std::string symbol(prim->attr("material"));
        int32_t slot = 0;
        if (!symbol.empty())
        {
            auto [it, added] = slotOf.emplace(symbol, int32_t(slotNames.size()));
            if (added)
                slotNames.push_back(symbol);
            slot = it->second;
        }

        std::vector<std::vector<int64_t>> polygons;
        if (tris)
        {
            const std::vector<int64_t> p = prim->child("p") ? xml::integers(prim->child("p")->text) : std::vector<int64_t>();
            for (size_t i = 0; i + stride * 3 <= p.size(); i += stride * 3)
                polygons.emplace_back(p.begin() + std::ptrdiff_t(i), p.begin() + std::ptrdiff_t(i + stride * 3));
        }
        else if (list)
        {
            const std::vector<int64_t> p = prim->child("p") ? xml::integers(prim->child("p")->text) : std::vector<int64_t>();
            const std::vector<int64_t> vc = prim->child("vcount") ? xml::integers(prim->child("vcount")->text) : std::vector<int64_t>();
            size_t at = 0;
            for (int64_t n : vc)
            {
                const size_t len = size_t(std::max<int64_t>(n, 0)) * stride;
                if (at + len > p.size())
                    break;
                polygons.emplace_back(p.begin() + std::ptrdiff_t(at), p.begin() + std::ptrdiff_t(at + len));
                at += len;
            }
        }
        else
        {
            for (const Element* pe : prim->all("p"))
                polygons.push_back(xml::integers(pe->text));
            if (prim->child("ph"))
                r.warnings.push_back(std::format("geometry '{}': polygons with holes imported without their holes", gid));
        }

        for (const std::vector<int64_t>& poly : polygons)
        {
            const size_t n = poly.size() / stride;
            if (n < 3)
                continue;
            std::vector<Index> pts;
            for (size_t k = 0; k < n; ++k)
            {
                const int64_t v = poly[k * stride + vertexOffset];
                pts.push_back(v >= 0 && size_t(v) < pointCount ? Index(v) : kInvalid);
            }
            if (std::find(pts.begin(), pts.end(), kInvalid) != pts.end())
            {
                ++degenerate;
                continue;
            }
            const Index f = m->addFace(std::span<const Index>(pts.data(), pts.size()));
            if (f == kInvalid)
            {
                ++degenerate;
                continue;
            }
            faceOrder.push_back(f);
            faceSlot.push_back(slot);
            for (size_t k = 0; k < n; ++k)
            {
                Corner c;
                auto idx = [&](const Input& in) { return poly[k * stride + in.offset]; };
                auto vertexIdx = [&](const Input& in) { return in.perVertex ? int64_t(pts[k]) : idx(in); };
                if (nrmIn && nrmIn->src)
                {
                    const int64_t i = vertexIdx(*nrmIn);
                    glm::dvec3 nv = nm * glm::dvec3(nrmIn->src->at(size_t(i), 0), nrmIn->src->at(size_t(i), 1), nrmIn->src->at(size_t(i), 2));
                    const double len = glm::length(nv);
                    c.normal = glm::vec3(len > 0.0 ? nv / len : glm::dvec3(0, 1, 0));
                    c.hasNormal = true;
                }
                for (const Input* in : uvIn)
                {
                    const int64_t i = vertexIdx(*in);
                    c.uv.emplace_back(float(in->src->at(size_t(i), 0)), float(in->src->stride > 1 ? in->src->at(size_t(i), 1) : 0.0));
                }
                for (const Input* in : colIn)
                {
                    const int64_t i = vertexIdx(*in);
                    const size_t st = in->src->stride;
                    c.color.emplace_back(float(in->src->at(size_t(i), 0)), float(st > 1 ? in->src->at(size_t(i), 1) : 0.0),
                                         float(st > 2 ? in->src->at(size_t(i), 2) : 0.0), float(st > 3 ? in->src->at(size_t(i), 3) : 1.0));
                }
                corners.push_back(std::move(c));
            }
        }
    }
    if (degenerate)
        r.warnings.push_back(std::format("geometry '{}': {} degenerate or invalid polygon(s) skipped", gid, degenerate));

    // Corner data in creation order: each face's corners from its first corner on.
    auto forCorners = [&](auto&& fn) {
        size_t at = 0;
        for (Index f : faceOrder)
        {
            Index c = m->faceCorner(f);
            for (Index k = 0; k < m->faceSize(f); ++k, c = m->cornerNext(c))
                fn(c, corners[at++]);
        }
    };
    if (!slotNames.empty())
    {
        std::span<int32_t> mat = m->faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
        for (size_t i = 0; i < faceOrder.size(); ++i)
            mat[faceOrder[i]] = faceSlot[i];
    }
    if (anyNormals)
    {
        std::span<glm::vec3> nrm = m->cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
        forCorners([&](Index c, const Corner& cd) { nrm[c] = cd.hasNormal ? cd.normal : glm::vec3(0, 1, 0); });
    }
    for (size_t s = 0; s < uvSets; ++s)
    {
        std::span<glm::vec2> uv = m->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(int(s)));
        forCorners([&](Index c, const Corner& cd) { uv[c] = s < cd.uv.size() ? cd.uv[s] : glm::vec2(0.0f); });
    }
    for (size_t s = 0; s < colorSets; ++s)
    {
        const std::string name = geo::colors::uniqueName(*m, "colorSet");
        std::span<glm::vec4> col = geo::colors::add(*m, name);
        forCorners([&](Index c, const Corner& cd) { col[c] = s < cd.color.size() ? cd.color[s] : glm::vec4(1.0f); });
    }
    if (anyNormals)
        fbx::hardEdgesFromNormals(*m);

    m->removeAllLoose();
    if (m->hasDeleted())
    {
        geo::MeshRemap remap;
        m->compact(&remap);
        std::vector<int64_t> kept(m->pointCapacity(), -1);
        for (size_t p = 0; p < out->source.size() && p < remap.points.size(); ++p)
            if (remap.points[p] != kInvalid && remap.points[p] < kept.size())
                kept[remap.points[p]] = out->source[p];
        out->source = std::move(kept);
    }
    if (m->numFaces() == 0)
        r.warnings.push_back(std::format("geometry '{}' has no polygons to import", gid));
    else
        out->mesh = std::move(m);
    return out;
}

// Materials ----------------------------------------------------------------------------------------------------------

std::string Importer::imagePath(const Element* effect, std::string_view textureRef) const
{
    // texture= names a sampler2D newparam (its source a surface newparam with the image) or an image directly.
    std::string ref(textureRef);
    for (int hop = 0; hop < 3; ++hop)
    {
        const Element* param = nullptr;
        for (const Element* scope = effect; scope && !param; scope = nullptr)
            for (const Element* p : {effect->find("profile_COMMON")})
                if (p)
                    for (const Element* np : p->all("newparam"))
                        if (np->attr("sid") == ref)
                            param = np;
        if (!param)
            for (const Element* np : effect->all("newparam"))
                if (np->attr("sid") == ref)
                    param = np;
        if (!param)
            break;
        if (const Element* sampler = param->child("sampler2D"))
        {
            if (const Element* src = sampler->child("source"))
                ref = src->text;
            else if (const Element* inst = sampler->child("instance_image"))
                ref = std::string(stripHash(inst->attr("url")));
            continue;
        }
        if (const Element* surface = param->child("surface"))
            if (const Element* init = surface->child("init_from"))
            {
                ref = init->text;
                break;
            }
        break;
    }
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.pop_back();
        size_t b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
            ++b;
        return s.substr(b);
    };
    ref = trim(ref);
    const Element* image = lookup(ref);
    if (!image || image->name != "image")
        return {};
    const Element* init = image->child("init_from");
    if (init && init->child("ref"))
        init = init->child("ref");
    if (!init)
        return {};
    fs::path p = pathOf(decodeUri(trim(init->text)));
    if (p.is_relative())
        p = dir / p;
    std::error_code ec;
    return textOf(fs::weakly_canonical(p, ec).empty() ? p : fs::weakly_canonical(p, ec));
}

void Importer::importMaterials()
{
    const Element* lib = root.child("library_materials");
    if (!lib || !opts.materials)
        return;
    for (const Element* mat : lib->all("material"))
    {
        const std::string id(mat->attr("id"));
        std::string wanted = fbx::safeName(mat->has("name") ? mat->attr("name") : mat->attr("id"), "material");
        Node* n = materials::find(graph, wanted);
        if (!n)
        {
            n = materials::create(graph, wanted);
            r.materials.push_back(n->name());
        }
        else
        {
            materialName[id] = n->name();
            continue;
        }
        materialName[id] = n->name();
        const Element* inst = mat->child("instance_effect");
        const Element* effect = inst ? lookup(inst->attr("url")) : nullptr;
        const Element* profile = effect ? effect->child("profile_COMMON") : nullptr;
        const Element* tech = profile ? profile->child("technique") : nullptr;
        const Element* shading = nullptr;
        if (tech)
            for (const char* model : {"phong", "blinn", "lambert", "constant"})
                if ((shading = tech->child(model)))
                    break;
        if (!shading)
            continue;
        const NodeId mid = n->id();
        auto colorOf = [&](const Element* slot, glm::dvec3* c, std::string* map) {
            if (!slot)
                return false;
            if (const Element* col = slot->child("color"))
            {
                const std::vector<double> v = xml::numbers(col->text);
                if (v.size() >= 3)
                    *c = glm::dvec3(v[0], v[1], v[2]);
                return true;
            }
            if (const Element* tex = slot->child("texture"))
            {
                *map = imagePath(effect, tex->attr("texture"));
                return true;
            }
            return false;
        };
        glm::dvec3 diffuse(0.5), emission(0.0);
        std::string diffuseMap, emissionMap;
        if (colorOf(shading->child("diffuse"), &diffuse, &diffuseMap))
        {
            graph.setValue({mid, material::kBaseColor, -1}, diffuseMap.empty() ? diffuse : glm::dvec3(1.0));
            if (!diffuseMap.empty())
                graph.setValue({mid, material::kBaseColorMap, -1}, diffuseMap);
        }
        if (colorOf(shading->child("emission"), &emission, &emissionMap))
        {
            graph.setValue({mid, material::kEmissiveColor, -1}, emissionMap.empty() ? emission : glm::dvec3(1.0));
            if (!emissionMap.empty())
                graph.setValue({mid, material::kEmissiveMap, -1}, emissionMap);
        }
        if (const Element* transparency = shading->child("transparency"))
            if (const Element* f = transparency->child("float"))
            {
                double opacity = 1.0;
                const std::vector<double> v = xml::numbers(f->text);
                const Element* transparent = shading->child("transparent");
                const std::string_view mode = transparent ? transparent->attr("opaque") : std::string_view("A_ONE");
                const Element* tc = transparent ? transparent->child("color") : nullptr;
                const std::vector<double> c = tc ? xml::numbers(tc->text) : std::vector<double>{1, 1, 1, 1};
                if (!v.empty() && c.size() >= 4)
                    opacity = mode == "RGB_ZERO" ? 1.0 - v[0] * (c[0] + c[1] + c[2]) / 3.0 : c[3] * v[0];
                if (opacity < 1.0 - 1e-6 && opacity >= 0.0)
                    graph.setValue({mid, material::kOpacity, -1}, opacity);
            }
        if (const Element* bump = effect->find("bump"))
            if (const Element* tex = bump->child("texture"))
                if (std::string p = imagePath(effect, tex->attr("texture")); !p.empty())
                    graph.setValue({mid, material::kNormalMap, -1}, p);
    }
}

// Scene --------------------------------------------------------------------------------------------------------------

std::vector<const Element*> Importer::skinJoints(const Element* controller, const Element* instance)
{
    std::vector<const Element*> out;
    const Element* skin = controller ? controller->child("skin") : nullptr;
    const Element* jointsEl = skin ? skin->child("joints") : nullptr;
    if (!jointsEl)
        return out;
    const Element* jointSrc = nullptr;
    for (const Element* in : jointsEl->all("input"))
        if (in->attr("semantic") == "JOINT")
            jointSrc = lookup(in->attr("source"));
    if (!jointSrc)
        return out;
    const bool byIdRef = jointSrc->child("IDREF_array") != nullptr;
    const std::vector<std::string> names = source(jointSrc).names;
    std::vector<const Element*> skeletons;
    if (instance)
        for (const Element* s : instance->all("skeleton"))
            if (const Element* e = lookup(s->text))
                skeletons.push_back(e);
    std::function<const Element*(const Element*, std::string_view)> bySid = [&](const Element* e, std::string_view sid) -> const Element* {
        if (e->name == "node" && e->attr("sid") == sid)
            return e;
        for (const auto& c : e->children)
            if (c->name == "node")
                if (const Element* f = bySid(c.get(), sid))
                    return f;
        return nullptr;
    };
    for (const std::string& name : names)
    {
        const Element* found = nullptr;
        if (byIdRef)
            found = lookup(name);
        for (const Element* s : skeletons)
            if (!found)
                found = bySid(s, name);
        if (!found)
            for (const auto& [e, fn] : nodes)
                if (e->attr("sid") == name)
                {
                    found = e;
                    break;
                }
        if (!found)
            if (const Element* e = lookup(name); e && e->name == "node")
                found = e;
        if (!found)
            for (const auto& [e, fn] : nodes)
                if (e->attr("name") == name)
                {
                    found = e;
                    break;
                }
        out.push_back(found);
    }
    return out;
}

void Importer::markJoints(const Element* visualScene)
{
    std::function<void(const Element*)> walk = [&](const Element* e) {
        for (const auto& c : e->children)
            if (c->name == "node")
            {
                FileNode& fn = nodes[c.get()];
                fn.e = c.get();
                fn.joint = c->attr("type") == "JOINT";
                walk(c.get());
            }
            else if (c->name == "instance_node")
                if (const Element* ref = lookup(c->attr("url")))
                {
                    FileNode& fn = nodes[ref];
                    fn.e = ref;
                    fn.joint = ref->attr("type") == "JOINT";
                    walk(ref);
                }
    };
    walk(visualScene);
    // Every node a skin binds to is a joint, whatever its type says.
    std::function<void(const Element*)> bound = [&](const Element* e) {
        for (const auto& c : e->children)
        {
            if (c->name == "instance_controller")
                for (const Element* j : skinJoints(lookup(c->attr("url")), c.get()))
                    if (j && nodes.count(j))
                        nodes[j].joint = true;
            bound(c.get());
        }
    };
    bound(visualScene);
}

NodeId Importer::attachMesh(NodeId node, const std::shared_ptr<BuiltMesh>& built, const Element* instance, bool own, const std::string& name)
{
    // Symbols bound to materials become the slot names.
    MeshPtr mesh = built->mesh;
    if (instance && !mesh->materialNames().empty())
    {
        std::map<std::string, std::string> bound;
        if (const Element* bm = instance->child("bind_material"))
            if (const Element* tc = bm->child("technique_common"))
                for (const Element* im : tc->all("instance_material"))
                {
                    const std::string target(stripHash(im->attr("target")));
                    auto it = materialName.find(target);
                    const Element* mat = lookup(target);
                    bound[std::string(im->attr("symbol"))] = it != materialName.end() ? it->second
                                                             : mat                       ? fbx::safeName(mat->has("name") ? mat->attr("name") : mat->attr("id"), "material")
                                                                                         : std::string(im->attr("symbol"));
                }
        auto copy = std::make_shared<geo::Mesh>(*mesh);
        for (std::string& slot : copy->materialNames())
            if (auto it = bound.find(slot); it != bound.end())
                slot = it->second;
        mesh = copy;
    }
    NodeId target = node;
    if (!own)
    {
        Node* n = graph.createNode(*types::find("mesh"), fbx::safeName(name, "Mesh"), node, false);
        target = n->id();
        r.nodes.push_back(target);
    }
    graph.setValue({target, MeshNode::kInMesh, -1}, mesh);
    return target;
}

void Importer::create(const Element* e, NodeId parent, const Element* via)
{
    FileNode& fn = nodes[e];
    fn.e = e;
    for (const auto& c : e->children)
    {
        const std::string& tag = c->name;
        StackItem item{StackItem::Matrix, std::string(c->attr("sid")), xml::numbers(c->text)};
        if (tag == "matrix")
            item.kind = StackItem::Matrix;
        else if (tag == "translate")
            item.kind = StackItem::Translate;
        else if (tag == "rotate")
            item.kind = StackItem::Rotate;
        else if (tag == "scale")
            item.kind = StackItem::Scale;
        else if (tag == "lookat")
            item.kind = StackItem::LookAt;
        else if (tag == "skew")
            item.kind = StackItem::Skew;
        else
            continue;
        fn.stack.push_back(std::move(item));
    }

    // The node's first geometry or controller goes on the node itself, the rest on child meshes.
    struct Geo
    {
        const Element* instance;
        const Element* controller;
        std::shared_ptr<BuiltMesh> built;
    };
    std::vector<Geo> geos;
    for (const auto& c : e->children)
    {
        if (c->name == "instance_geometry")
        {
            if (const Element* g = lookup(c->attr("url")))
                if (auto b = buildGeometry(g); b->mesh)
                    geos.push_back({c.get(), nullptr, b});
        }
        else if (c->name == "instance_controller")
        {
            const Element* ctrl = lookup(c->attr("url"));
            const Element* skin = ctrl ? ctrl->child("skin") : nullptr;
            const Element* morph = ctrl ? ctrl->child("morph") : nullptr;
            const Element* base = skin ? lookup(skin->attr("source")) : morph ? lookup(morph->attr("source")) : nullptr;
            // A skin over a morph: the morph's base mesh.
            if (base && base->name == "controller" && base->child("morph"))
            {
                r.skipped.push_back(std::format("morph controller '{}' (its base mesh is imported)", base->attr("id")));
                base = lookup(base->child("morph")->attr("source"));
            }
            if (morph)
                r.skipped.push_back(std::format("morph controller '{}' (its base mesh is imported)", ctrl->attr("id")));
            if (base && base->name == "geometry")
                if (auto b = buildGeometry(base); b->mesh)
                    geos.push_back({c.get(), skin ? ctrl : nullptr, b});
        }
        else if (c->name == "instance_light" || c->name == "instance_camera")
            r.skipped.push_back(std::format("{} on node '{}'", c->name, e->attr("name")));
    }

    const bool isJoint = fn.joint && types::find("joint");
    const bool isMesh = !isJoint && !geos.empty();
    const NodeType& type = isJoint ? *types::find("joint") : isMesh ? *types::find("mesh") : *types::find("transform");
    std::string raw(e->has("name") ? e->attr("name") : e->attr("id"));
    Node* n = graph.createNode(type, fbx::safeName(raw, isJoint ? "joint" : isMesh ? "Mesh" : "Transform"), parent, false);
    const NodeId id = n->id();
    fn.id = id;
    r.nodes.push_back(id);
    if (parent == opts.parent)
        r.roots.push_back(id);

    const glm::dmat4 local = toLila(fn.localAt(0.0 - 1e30 * 0.0));
    const MayaElements maya = mayaElements(fn.stack);
    if (maya.found)
    {
        fn.order = maya.order;
        graph.setValue({id, Transform::kRotateOrder, -1}, int64_t(maya.order));
    }
    if (isJoint)
    {
        r.joints.push_back(id);
        glm::dvec3 jo(0.0);
        if (maya.found)
        {
            // Maya's elements: the file's own jointOrient stays, so animated rotate channels mean what they did.
            fn.pre = F * maya.jointOrient * glm::transpose(F);
            jo = clean(matrixToEuler(fn.pre, RotateOrder::XYZ));
        }
        else
        {
            // The rest rotation becomes jointOrient, so the joint rests at zero rotation the way rigs expect.
            jo = clean(fbx::decompose(local).r);
        }
        fn.pre = glm::dmat3(eulerToMatrix(jo, RotateOrder::XYZ));
        fn.rest = fbx::decomposeWith(local, fn.pre, fn.order, maya.found ? maya.rotate : glm::dvec3(0.0));
        fn.rest.r = clean(fn.rest.r);
        graph.setValue({id, Joint::kJointOrient, -1}, jo);
    }
    else
        fn.rest = fbx::decompose(local);
    if (fn.rest.sheared)
        r.warnings.push_back(std::format("{}: shear dropped from its transform", n->path()));
    graph.setValue({id, Transform::kTranslate, -1}, clean(fn.rest.t));
    graph.setValue({id, Transform::kRotate, -1}, clean(fn.rest.r));
    graph.setValue({id, Transform::kScale, -1}, fn.rest.s);

    for (size_t k = 0; k < geos.size(); ++k)
    {
        const Geo& g = geos[k];
        const bool own = isMesh && k == 0;
        const NodeId meshId = attachMesh(id, g.built, g.instance, own, n->name() + std::format("Shape{}", k ? std::to_string(k) : ""));
        if (g.controller)
            skins.push_back({meshId, g.controller, g.instance, g.built});
    }

    for (const auto& c : e->children)
        if (c->name == "node")
            create(c.get(), id, nullptr);
        else if (c->name == "instance_node")
            if (const Element* ref = lookup(c->attr("url")); ref && ref != via)
                create(ref, id, e);
}

void Importer::buildSkins()
{
    if (skins.empty())
        return;
    if (!opts.skins || !types::find("skinCluster"))
    {
        for (const SkinUse& s : skins)
            r.skipped.push_back(std::format("skin on {} (mesh imported unskinned)", graph.path(s.mesh)));
        return;
    }
    graph.evaluate();
    const glm::dmat4 parentWorld = opts.parent != kNoNode ? graph.node(opts.parent)->as<glm::dmat4>(Transform::kWorldMatrix) : glm::dmat4(1.0);
    const glm::dmat4 parentInv = glm::inverse(parentWorld);
    for (const SkinUse& use : skins)
    {
        const Element* skin = use.controller->child("skin");
        const std::string meshName = graph.path(use.mesh);
        glm::dmat4 bsm(1.0);
        if (const Element* b = skin->child("bind_shape_matrix"))
            if (const std::vector<double> v = xml::numbers(b->text); v.size() >= 16)
                bsm = matrixFrom(v.data());

        // Bind shape to skin space, then into the mesh node's object space.
        const glm::dmat4 W = graph.node(use.mesh)->as<glm::dmat4>(Transform::kWorldMatrix);
        const glm::dmat4 toObject = glm::inverse(W) * parentWorld * toLila(bsm);
        const PlugRef top{use.mesh, MeshNode::kInMesh, -1};
        if (!nearIdentity(toObject))
        {
            const PlugValue stored = graph.storedValue(top);
            const MeshPtr* held = std::get_if<MeshPtr>(&stored);
            if (!held || !*held)
                continue;
            MeshPtr src = *held;
            auto moved = std::make_shared<geo::Mesh>(*src);
            for (Index p = 0; p < moved->pointCapacity(); ++p)
                if (moved->pointValid(p))
                    moved->setPosition(p, glm::dvec3(toObject * glm::dvec4(moved->position(p), 1.0)));
            if (std::span<glm::vec3> nrm = moved->cornerAttrs().get<glm::vec3>(geo::attr::kNormal); !nrm.empty())
            {
                const glm::dmat3 nmat = glm::inverse(glm::transpose(glm::dmat3(toObject)));
                for (glm::vec3& v : nrm)
                {
                    const glm::dvec3 d = nmat * glm::dvec3(v);
                    const double len = glm::length(d);
                    v = glm::vec3(len > 0.0 ? d / len : glm::dvec3(0, 1, 0));
                }
            }
            graph.setValue(top, MeshPtr(moved));
        }

        const std::vector<const Element*> joints = skinJoints(use.controller, use.instance);
        std::vector<double> ibm;
        if (const Element* je = skin->child("joints"))
            for (const Element* in : je->all("input"))
                if (in->attr("semantic") == "INV_BIND_MATRIX")
                    ibm = source(lookup(in->attr("source"))).values;

        Node* sc = graph.createNode(*types::find("skinCluster"), "SkinCluster", kNoNode, true);
        const NodeId skinId = sc->id();
        Node* src = graph.createNode(*types::find("meshSource"), "meshSource", kNoNode, true);
        graph.setValue({src->id(), MeshSourceNode::kMesh, -1}, graph.storedValue(top));
        graph.setValue(top, MeshPtr());
        graph.connect({src->id(), MeshSourceNode::kOutMesh, -1}, {skinId, SkinClusterNode::kInMesh, -1});
        graph.connect({skinId, SkinClusterNode::kOutMesh, -1}, top);
        graph.setValue({skinId, SkinClusterNode::kGeomMatrix}, W);

        std::vector<int64_t> influenceOf(joints.size(), -1);
        for (size_t j = 0; j < joints.size(); ++j)
        {
            auto it = joints[j] ? nodes.find(joints[j]) : nodes.end();
            if (it == nodes.end() || it->second.id == kNoNode)
            {
                r.warnings.push_back(std::format("{}: skin joint {} is not in the imported scene; its weights are dropped", meshName, j));
                continue;
            }
            const uint32_t index = skin::addInfluence(graph, skinId, it->second.id);
            influenceOf[j] = index;
            const glm::dmat4 bind = ibm.size() >= (j + 1) * 16 ? matrixFrom(&ibm[j * 16]) : glm::dmat4(1.0);
            for (const skin::Influence& inf : skin::influences(graph, skinId))
                if (inf.index == index)
                    graph.setValue({inf.link, SkinInfluenceNode::kBindPreMatrix, -1}, toLila(bind) * parentInv);
        }

        // vertex_weights: per position index, (joint, weight) pairs by the inputs' offsets; joint -1 is the bind shape.
        std::vector<std::vector<std::pair<int64_t, double>>> perPosition;
        if (const Element* vw = skin->child("vertex_weights"))
        {
            size_t jointOffset = 0, weightOffset = 1, stride = 0;
            std::vector<double> weights;
            for (const Element* in : vw->all("input"))
            {
                const size_t off = size_t(std::atoll(std::string(in->attr("offset")).c_str()));
                stride = std::max(stride, off + 1);
                if (in->attr("semantic") == "JOINT")
                    jointOffset = off;
                else if (in->attr("semantic") == "WEIGHT")
                {
                    weightOffset = off;
                    weights = source(lookup(in->attr("source"))).values;
                }
            }
            const std::vector<int64_t> vc = vw->child("vcount") ? xml::integers(vw->child("vcount")->text) : std::vector<int64_t>();
            const std::vector<int64_t> v = vw->child("v") ? xml::integers(vw->child("v")->text) : std::vector<int64_t>();
            size_t at = 0;
            for (int64_t n : vc)
            {
                auto& list = perPosition.emplace_back();
                for (int64_t k = 0; k < n && at + stride <= v.size(); ++k, at += stride)
                {
                    const int64_t joint = v[at + jointOffset];
                    const int64_t wi = v[at + weightOffset];
                    const double weight = wi >= 0 && size_t(wi) < weights.size() ? weights[size_t(wi)] : 0.0;
                    list.push_back({joint, weight});
                }
            }
        }

        MeshPtr bindMesh = skin::inputMesh(graph, skinId);
        WeightTable t;
        t.points.resize(bindMesh ? bindMesh->pointCapacity() : 0);
        size_t maxUsed = 0;
        for (size_t p = 0; p < t.points.size() && p < use.built->source.size(); ++p)
        {
            const int64_t pos = use.built->source[p];
            if (pos < 0 || size_t(pos) >= perPosition.size())
                continue;
            std::map<uint32_t, double> sum;
            for (const auto& [j, w] : perPosition[size_t(pos)])
                if (w > 0.0 && j >= 0 && size_t(j) < influenceOf.size() && influenceOf[size_t(j)] >= 0)
                    sum[uint32_t(influenceOf[size_t(j)])] += w;
            double total = 0.0;
            for (const auto& [inf, w] : sum)
                total += w;
            for (const auto& [inf, w] : sum)
                t.points[p].push_back({inf, float(total > 0.0 ? w / total : w)});
            maxUsed = std::max(maxUsed, t.points[p].size());
        }
        graph.setValue({skinId, SkinClusterNode::kMaxInfluences}, int64_t(std::max<size_t>(4, maxUsed)));
        skin::setWeights(graph, skinId, t);
        r.skins.push_back(skinId);
    }
}

// Animation ----------------------------------------------------------------------------------------------------------

void Importer::readAnimations()
{
    const Element* lib = root.child("library_animations");
    if (!lib)
        return;
    std::unordered_map<std::string, FileNode*> byNodeId;
    for (auto& [e, fn] : nodes)
        if (e->has("id"))
            byNodeId[std::string(e->attr("id"))] = &fn;
    size_t unresolved = 0;
    std::function<void(const Element*)> walk = [&](const Element* anim) {
        for (const Element* ch : anim->all("channel"))
        {
            const std::string target(ch->attr("target"));
            const size_t slash = target.find('/');
            auto it = slash == std::string::npos ? byNodeId.end() : byNodeId.find(target.substr(0, slash));
            if (it == byNodeId.end())
            {
                ++unresolved;
                continue;
            }
            FileNode& fn = *it->second;
            std::string rest = target.substr(slash + 1);
            // sid, then .member or (i) or (i)(j).
            int component = -1;
            std::string sid = rest;
            if (const size_t dot = rest.find('.'); dot != std::string::npos)
            {
                sid = rest.substr(0, dot);
                const std::string member = rest.substr(dot + 1);
                component = member == "X" ? 0 : member == "Y" ? 1 : member == "Z" ? 2 : member == "ANGLE" ? 3 : -2;
            }
            else if (const size_t paren = rest.find('('); paren != std::string::npos)
            {
                sid = rest.substr(0, paren);
                const std::vector<int64_t> idx = xml::integers([&] {
                    std::string s = rest.substr(paren);
                    for (char& c : s)
                        if (c == '(' || c == ')')
                            c = ' ';
                    return s;
                }());
                // (i)(j) is column i, row j of a matrix; COLLADA lists matrices row-major.
                component = idx.size() == 2 ? int(idx[1] * 4 + idx[0]) : idx.size() == 1 ? int(idx[0]) : -2;
            }
            size_t item = SIZE_MAX;
            for (size_t i = 0; i < fn.stack.size(); ++i)
                if (fn.stack[i].sid == sid)
                    item = i;
            if (item == SIZE_MAX || component == -2)
            {
                ++unresolved;
                continue;
            }
            if (component == 3 && fn.stack[item].kind != StackItem::Rotate)
                component = -2;
            if (component == -2)
            {
                ++unresolved;
                continue;
            }
            const Element* sampler = lookup(ch->attr("source"));
            if (!sampler)
                continue;
            Channel c;
            c.item = item;
            c.component = component;
            for (const Element* in : sampler->all("input"))
            {
                const std::string_view sem = in->attr("semantic");
                const Source s = source(lookup(in->attr("source")));
                if (sem == "INPUT")
                    c.times = s.values;
                else if (sem == "OUTPUT")
                {
                    c.values = s.values;
                    c.stride = s.stride;
                }
                else if (sem == "INTERPOLATION")
                    c.interp = s.names;
                else if (sem == "IN_TANGENT")
                {
                    c.inTan = s.values;
                    c.tanStride = s.stride;
                }
                else if (sem == "OUT_TANGENT")
                    c.outTan = s.values;
            }
            if (c.times.empty() || c.values.size() < c.times.size() * c.stride)
                continue;
            fn.channels.push_back(std::move(c));
        }
        for (const Element* sub : anim->all("animation"))
            walk(sub);
    };
    for (const Element* a : lib->all("animation"))
        walk(a);
    if (unresolved)
        r.skipped.push_back(std::format("{} animation channel(s) on targets Lila doesn't read", unresolved));
}

double guessFps(const std::vector<double>& times)
{
    double step = std::numeric_limits<double>::max();
    for (size_t i = 1; i < times.size(); ++i)
        if (times[i] - times[i - 1] > 1e-6)
            step = std::min(step, times[i] - times[i - 1]);
    if (step == std::numeric_limits<double>::max())
        return 0.0;
    for (double fps : {24.0, 25.0, 30.0, 48.0, 50.0, 60.0, 120.0, 23.976, 29.97, 59.94, 15.0, 12.0})
    {
        const double frames = step * fps;
        if (std::abs(frames - std::round(frames)) < 0.01 && std::round(frames) >= 1.0)
            return fps;
    }
    return 0.0;
}

void Importer::importAnimation()
{
    std::vector<FileNode*> animated;
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
    std::vector<double> allTimes;
    for (auto& [e, fn] : nodes)
        if (!fn.channels.empty() && fn.id != kNoNode)
        {
            animated.push_back(&fn);
            for (const Channel& c : fn.channels)
            {
                lo = std::min(lo, c.times.front());
                hi = std::max(hi, c.times.back());
                allTimes.insert(allTimes.end(), c.times.begin(), c.times.end());
            }
        }
    if (animated.empty())
        return;
    std::sort(allTimes.begin(), allTimes.end());
    double fps = opts.fps > 0.0 ? opts.fps : guessFps(allTimes);
    if (fps <= 0.0)
        fps = anim::playback().fps > 0.0 ? anim::playback().fps : 30.0;
    r.fps = fps;

    struct Range
    {
        std::string name;
        double start, end;
    };
    std::vector<Range> ranges;
    if (const Element* clips = root.child("library_animation_clips"))
        for (const Element* c : clips->all("animation_clip"))
        {
            const std::string name(c->has("name") ? c->attr("name") : c->attr("id"));
            r.clips.push_back(name);
            const double s = c->has("start") ? std::atof(std::string(c->attr("start")).c_str()) : lo;
            const double e = c->has("end") ? std::atof(std::string(c->attr("end")).c_str()) : hi;
            ranges.push_back({name, s, std::max(s, e)});
        }
    if (ranges.size() < 2)
        ranges = {{ranges.empty() ? std::string() : ranges[0].name, lo, hi}};
    r.start = ranges[0].start;
    r.end = ranges[0].end;

    auto bake = [&](double start, double end) {
        std::vector<double> times;
        const long long count = std::llround((end - start) * fps);
        for (long long f = 0; f <= count; ++f)
            times.push_back(start + double(f) / fps);
        static const char* kAxis[3] = {"X", "Y", "Z"};
        for (FileNode* fn : animated)
        {
            std::array<anim::Curve, 3> t, s;
            anim::RotationCurve rc;
            rc.interp = anim::RotInterp::Slerp;
            rc.order = fn->order;
            std::array<anim::Curve, 3> euler;
            glm::dvec3 ref = fn->rest.r;
            for (double time : times)
            {
                const glm::dmat4 local = toLila(fn->localAt(time));
                const fbx::Trs trs = fn->joint ? fbx::decomposeWith(local, fn->pre, fn->order, ref) : fbx::decompose(local);
                ref = fn->joint ? trs.r : eulerNear(glm::dmat3(local) * glm::dmat3(glm::scale(glm::dmat4(1.0), 1.0 / trs.s)), fn->order, ref);
                for (int a = 0; a < 3; ++a)
                {
                    anim::Key k;
                    k.time = time;
                    k.in = k.out = anim::Tangent::Linear;
                    k.value = trs.t[a];
                    t[size_t(a)].keys.push_back(k);
                    k.value = trs.s[a];
                    s[size_t(a)].keys.push_back(k);
                    k.value = ref[a];
                    euler[size_t(a)].keys.push_back(k);
                }
                anim::RotKey key = rc.makeKey(time, ref);
                key.in = key.out = anim::Tangent::Linear;
                rc.keys.push_back(key);
            }
            const std::string base = graph.node(fn->id)->name();
            auto still = [](const anim::Curve& c, double v) {
                return std::all_of(c.keys.begin(), c.keys.end(), [&](const anim::Key& k) { return std::abs(k.value - v) < 1e-6; });
            };
            for (int a = 0; a < 3; ++a)
            {
                if (!still(t[size_t(a)], fn->rest.t[a]) || ranges.size() > 1)
                {
                    fbx::connectCurve(graph, {fn->id, Transform::kTranslate, a}, "animCurveTL", t[size_t(a)], base + "_translate" + kAxis[a]);
                    ++r.curves;
                }
                if (!still(s[size_t(a)], fn->rest.s[a]) || ranges.size() > 1)
                {
                    fbx::connectCurve(graph, {fn->id, Transform::kScale, a}, "animCurveTU", s[size_t(a)], base + "_scale" + kAxis[a]);
                    ++r.curves;
                }
            }
            bool turns = ranges.size() > 1;
            for (int a = 0; a < 3 && !turns; ++a)
                turns = !still(euler[size_t(a)], fn->rest.r[a]);
            if (!turns)
                continue;
            const PlugRef rotate{fn->id, Transform::kRotate, -1};
            if (anim::keysAsRotation(graph, rotate))
            {
                fbx::connectRotationCurve(graph, fn->id, rc);
                ++r.curves;
            }
            else
                for (int a = 0; a < 3; ++a)
                {
                    fbx::connectCurve(graph, {fn->id, Transform::kRotate, a}, "animCurveTA", euler[size_t(a)], base + "_rotate" + kAxis[a]);
                    ++r.curves;
                }
        }
    };

    if (ranges.size() == 1)
    {
        bake(ranges[0].start, ranges[0].end);
        if (opts.setPlayback)
            fbx::setPlayback(fps, ranges[0].start, ranges[0].end);
        return;
    }
    // Several clips: one take each over its stretch of the timeline, the first left active.
    anim::TakeSettings settings;
    settings.fps = fps;
    if (r.roots.size() == 1)
        settings.root = graph.node(r.roots[0])->path();
    NodeId first = kNoNode;
    for (const Range& range : ranges)
    {
        settings.start = range.start;
        settings.end = range.end;
        const NodeId take = anim::createTake(graph, fbx::safeName(range.name, "Take"), settings);
        anim::setActiveTake(graph, take);
        bake(range.start, range.end);
        if (first == kNoNode)
            first = take;
    }
    anim::setActiveTake(graph, first);
    if (opts.setPlayback)
        fbx::setPlayback(fps, ranges[0].start, ranges[0].end);
}

void indexIds(const Element& e, std::unordered_map<std::string, const Element*>& out)
{
    if (e.has("id"))
        out.emplace(std::string(e.attr("id")), &e);
    for (const auto& c : e.children)
        indexIds(*c, out);
}
} // namespace

ImportResult importCollada(scene::Graph& graph, const fs::path& path, const ImportOptions& opts)
{
    LILA_ZONE();

    ImportResult r;
    std::string text;
    if (!readText(path, text))
    {
        r.error = "can't read " + textOf(path);
        return r;
    }
    std::string err;
    std::unique_ptr<xml::Element> doc = xml::parse(text, &err);
    if (!doc)
    {
        r.error = "not a valid XML file: " + err;
        return r;
    }
    if (doc->name != "COLLADA")
    {
        r.error = "not a COLLADA file (root element <" + doc->name + ">)";
        return r;
    }

    Importer im{graph, *doc, path.parent_path(), opts, r};
    indexIds(*doc, im.byId);

    // Axes and units: the file's up axis onto Y up, then Lila's axes as for glTF (Y up, front +Z).
    glm::dmat3 toYUp(1.0);
    r.upAxis = "Y_UP";
    if (const Element* asset = doc->child("asset"))
    {
        if (const Element* up = asset->child("up_axis"))
        {
            const std::vector<std::string> w = xml::words(up->text);
            r.upAxis = w.empty() ? "Y_UP" : w[0];
        }
        if (const Element* unit = asset->child("unit"); unit && unit->has("meter"))
            r.unitMeters = std::atof(std::string(unit->attr("meter")).c_str());
        if (const Element* contrib = asset->child("contributor"))
            if (const Element* tool = contrib->child("authoring_tool"))
                r.authoringTool = tool->text;
    }
    if (r.unitMeters <= 0.0)
        r.unitMeters = 1.0;
    if (r.upAxis == "Z_UP")
        toYUp = glm::dmat3(glm::dvec3(1, 0, 0), glm::dvec3(0, 0, -1), glm::dvec3(0, 1, 0));
    else if (r.upAxis == "X_UP")
        toYUp = glm::dmat3(glm::dvec3(0, 1, 0), glm::dvec3(-1, 0, 0), glm::dvec3(0, 0, 1));
    im.F = glm::transpose(fbx::profileBasis(fbx::AxisProfile::Ue6, math::upAxis())) * toYUp;
    im.C = glm::dmat4(im.F) * glm::scale(glm::dmat4(1.0), glm::dvec3(r.unitMeters * 100.0));
    im.Cinv = glm::inverse(im.C);

    const Element* sceneEl = doc->child("scene");
    const Element* ivs = sceneEl ? sceneEl->child("instance_visual_scene") : nullptr;
    const Element* visual = ivs ? im.lookup(ivs->attr("url")) : nullptr;
    if (!visual)
        if (const Element* lib = doc->child("library_visual_scenes"))
            visual = lib->child("visual_scene");
    if (!visual)
    {
        r.error = "the file has no visual scene";
        return r;
    }

    Graph::Batch batch(graph);
    im.importMaterials();
    im.markJoints(visual);
    for (const Element* n : visual->all("node"))
        im.create(n, opts.parent, nullptr);
    im.buildSkins();
    im.readAnimations();
    if (opts.animation)
        im.importAnimation();
    else if (doc->child("library_animations"))
        r.skipped.push_back("animations");
    if (doc->child("library_physics_scenes") || doc->child("library_physics_models"))
        r.skipped.push_back("physics");
    r.ok = true;
    return r;
}
} // namespace lila::collada
