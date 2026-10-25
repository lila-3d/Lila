#include "geo/PlyIO.h"

#include "geo/ColorSets.h"
#include "geo/Normals.h"
#include "geo/Primitives.h"

#include "core/Profile.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <unordered_map>

namespace lila::geo
{
namespace
{
enum class Type : uint8_t
{
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    F32,
    F64,
};

std::optional<Type> parseType(std::string_view s)
{
    if (s == "char" || s == "int8")
        return Type::I8;
    if (s == "uchar" || s == "uint8")
        return Type::U8;
    if (s == "short" || s == "int16")
        return Type::I16;
    if (s == "ushort" || s == "uint16")
        return Type::U16;
    if (s == "int" || s == "int32")
        return Type::I32;
    if (s == "uint" || s == "uint32")
        return Type::U32;
    if (s == "float" || s == "float32")
        return Type::F32;
    if (s == "double" || s == "float64")
        return Type::F64;
    return std::nullopt;
}

size_t typeSize(Type t)
{
    switch (t)
    {
    case Type::I8:
    case Type::U8: return 1;
    case Type::I16:
    case Type::U16: return 2;
    case Type::I32:
    case Type::U32:
    case Type::F32: return 4;
    case Type::F64: return 8;
    }
    return 0;
}

/** Integer color channels are normalized by their type's range; floats are already 0 to 1. */
float channelScale(Type t)
{
    switch (t)
    {
    case Type::U16: return 1.0f / 65535.0f;
    case Type::F32:
    case Type::F64: return 1.0f;
    default: return 1.0f / 255.0f;
    }
}

float srgbToLinear(float c)
{
    c = std::max(c, 0.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linearToSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

struct Property
{
    std::string name;
    Type type = Type::F32;
    bool list = false;
    Type countType = Type::U8;
};

struct Element
{
    std::string name;
    size_t count = 0;
    std::vector<Property> props;

    int find(std::initializer_list<std::string_view> names) const
    {
        for (std::string_view n : names)
            for (size_t i = 0; i < props.size(); ++i)
                if (props[i].name == n)
                    return int(i);
        return -1;
    }
};

enum class Format
{
    Ascii,
    Little,
    Big,
};

/** Reads one value at a time from the body in any of the three encodings. */
class Body
{
public:
    Body(std::string_view data, Format format) : mData(data), mFormat(format) {}

    bool ok() const { return mOk; }

    double value(Type t)
    {
        if (mFormat == Format::Ascii)
            return asciiValue();
        const size_t n = typeSize(t);
        if (mAt + n > mData.size())
        {
            mOk = false;
            return 0.0;
        }
        unsigned char b[8];
        std::memcpy(b, mData.data() + mAt, n);
        mAt += n;
        if ((mFormat == Format::Big) == (std::endian::native == std::endian::little))
            std::reverse(b, b + n);
        switch (t)
        {
        case Type::I8: return double(std::bit_cast<int8_t>(b[0]));
        case Type::U8: return double(b[0]);
        case Type::I16: return double(load<int16_t>(b));
        case Type::U16: return double(load<uint16_t>(b));
        case Type::I32: return double(load<int32_t>(b));
        case Type::U32: return double(load<uint32_t>(b));
        case Type::F32: return double(load<float>(b));
        case Type::F64: return load<double>(b);
        }
        return 0.0;
    }

private:
    template <class T> static T load(const unsigned char* b)
    {
        T v;
        std::memcpy(&v, b, sizeof(T));
        return v;
    }

    double asciiValue()
    {
        while (mAt < mData.size() && std::isspace(static_cast<unsigned char>(mData[mAt])))
            ++mAt;
        const char* first = mData.data() + mAt;
        const char* last = mData.data() + mData.size();
        if (first < last && *first == '+')
            ++first;
        double v = 0.0;
        auto [ptr, ec] = std::from_chars(first, last, v);
        if (ec != std::errc() || ptr == first)
        {
            mOk = false;
            return 0.0;
        }
        mAt = size_t(ptr - mData.data());
        return v;
    }

    std::string_view mData;
    Format mFormat;
    size_t mAt = 0;
    bool mOk = true;
};

struct Header
{
    Format format = Format::Ascii;
    std::vector<Element> elements;
    std::vector<std::string> textureFiles;
    size_t bodyStart = 0;
    std::string error;
};

Header parseHeader(std::string_view data)
{
    Header h;
    size_t at = 0;
    auto nextLine = [&](std::string_view& line) {
        if (at >= data.size())
            return false;
        size_t end = data.find('\n', at);
        if (end == std::string_view::npos)
            end = data.size();
        line = data.substr(at, end - at);
        at = std::min(end + 1, data.size());
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        return true;
    };
    auto words = [](std::string_view line) {
        std::vector<std::string_view> w;
        size_t i = 0;
        while (i < line.size())
        {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
                ++i;
            const size_t s = i;
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
                ++i;
            if (i > s)
                w.push_back(line.substr(s, i - s));
        }
        return w;
    };

    std::string_view line;
    if (!nextLine(line) || line != "ply")
    {
        h.error = "not a PLY file";
        return h;
    }
    bool sawFormat = false;
    while (nextLine(line))
    {
        std::vector<std::string_view> w = words(line);
        if (w.empty())
            continue;
        if (w[0] == "end_header")
        {
            h.bodyStart = at;
            if (!sawFormat)
                h.error = "no format line";
            return h;
        }
        if (w[0] == "comment" || w[0] == "obj_info")
        {
            if (w.size() >= 3 && (w[1] == "TextureFile" || w[1] == "texturefile"))
            {
                const size_t pos = line.find(w[1]) + w[1].size();
                std::string_view rest = line.substr(pos);
                while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())))
                    rest.remove_prefix(1);
                h.textureFiles.emplace_back(rest);
            }
            continue;
        }
        if (w[0] == "format" && w.size() >= 2)
        {
            sawFormat = true;
            if (w[1] == "ascii")
                h.format = Format::Ascii;
            else if (w[1] == "binary_little_endian")
                h.format = Format::Little;
            else if (w[1] == "binary_big_endian")
                h.format = Format::Big;
            else
            {
                h.error = "unknown format \"" + std::string(w[1]) + "\"";
                return h;
            }
            continue;
        }
        if (w[0] == "element" && w.size() >= 3)
        {
            Element& e = h.elements.emplace_back();
            e.name = std::string(w[1]);
            auto [p, ec] = std::from_chars(w[2].data(), w[2].data() + w[2].size(), e.count);
            if (ec != std::errc())
            {
                h.error = "bad element count for \"" + e.name + "\"";
                return h;
            }
            continue;
        }
        if (w[0] == "property" && !h.elements.empty())
        {
            Property p;
            if (w.size() >= 5 && w[1] == "list")
            {
                std::optional<Type> ct = parseType(w[2]), it = parseType(w[3]);
                if (!ct || !it)
                {
                    h.error = "unknown property type in \"" + std::string(line) + "\"";
                    return h;
                }
                p.list = true;
                p.countType = *ct;
                p.type = *it;
                p.name = std::string(w[4]);
            }
            else if (w.size() >= 3)
            {
                std::optional<Type> t = parseType(w[1]);
                if (!t)
                {
                    h.error = "unknown property type in \"" + std::string(line) + "\"";
                    return h;
                }
                p.type = *t;
                p.name = std::string(w[2]);
            }
            else
            {
                h.error = "bad property line \"" + std::string(line) + "\"";
                return h;
            }
            h.elements.back().props.push_back(std::move(p));
            continue;
        }
        h.error = "unexpected header line \"" + std::string(line) + "\"";
        return h;
    }
    h.error = "no end_header";
    return h;
}

/** One element's values: scalars as columns, lists flattened with offsets. Only wanted properties are kept. */
struct Columns
{
    std::vector<std::vector<double>> scalar; // per property, empty when unwanted or a list
    std::vector<std::vector<double>> listValues;
    std::vector<std::vector<uint32_t>> listOffsets; // count + 1 per list property kept
};

bool readElement(Body& body, const Element& e, const std::vector<bool>& want, Columns& out)
{
    const size_t np = e.props.size();
    out.scalar.assign(np, {});
    out.listValues.assign(np, {});
    out.listOffsets.assign(np, {});
    for (size_t i = 0; i < np; ++i)
    {
        if (!want[i])
            continue;
        if (e.props[i].list)
        {
            out.listOffsets[i].reserve(e.count + 1);
            out.listOffsets[i].push_back(0);
            out.listValues[i].reserve(e.count * 3);
        }
        else
            out.scalar[i].reserve(e.count);
    }
    for (size_t k = 0; k < e.count; ++k)
    {
        for (size_t i = 0; i < np; ++i)
        {
            const Property& p = e.props[i];
            if (!p.list)
            {
                const double v = body.value(p.type);
                if (want[i])
                    out.scalar[i].push_back(v);
                continue;
            }
            const double n = body.value(p.countType);
            if (!body.ok() || n < 0.0 || n > 1e7)
                return false;
            for (size_t j = 0; j < size_t(n); ++j)
            {
                const double v = body.value(p.type);
                if (want[i])
                    out.listValues[i].push_back(v);
            }
            if (want[i])
                out.listOffsets[i].push_back(uint32_t(out.listValues[i].size()));
        }
        if (!body.ok())
            return false;
    }
    return true;
}

bool isBuiltinVertexProperty(std::string_view n)
{
    static constexpr std::string_view known[] = {"x", "y", "z", "nx", "ny", "nz", "red", "green", "blue", "alpha", "r", "g", "b", "a",
                                                 "diffuse_red", "diffuse_green", "diffuse_blue", "diffuse_alpha", "s", "t", "u", "v",
                                                 "texture_u", "texture_v", "texture_s", "texture_t"};
    return std::ranges::find(known, n) != std::end(known);
}

bool isBuiltinFaceProperty(std::string_view n)
{
    static constexpr std::string_view known[] = {"vertex_indices", "vertex_index", "texcoord", "red", "green", "blue", "alpha", "r", "g", "b", "a"};
    return std::ranges::find(known, n) != std::end(known);
}

/** A property name usable as a Lila attribute, or empty when it would collide with a built-in layer. */
std::string extraAttributeName(std::string_view name)
{
    if (name == attr::kNormal || name == attr::kColor || name == attr::kMaterial || name == attr::kHard || name.starts_with("uv") ||
        name.starts_with(colors::kLayerPrefix))
        return "ply_" + std::string(name);
    return std::string(name);
}

struct ColorProps
{
    int r = -1, g = -1, b = -1, a = -1;
    bool any() const { return r >= 0 && g >= 0 && b >= 0; }
};

ColorProps colorProps(const Element& e)
{
    return {e.find({"red", "r", "diffuse_red"}), e.find({"green", "g", "diffuse_green"}), e.find({"blue", "b", "diffuse_blue"}),
            e.find({"alpha", "a", "diffuse_alpha"})};
}

glm::vec4 colorAt(const Element& e, const Columns& c, const ColorProps& p, size_t k)
{
    auto ch = [&](int i) { return float(c.scalar[size_t(i)][k]) * channelScale(e.props[size_t(i)].type); };
    return glm::vec4(srgbToLinear(ch(p.r)), srgbToLinear(ch(p.g)), srgbToLinear(ch(p.b)), p.a >= 0 ? std::clamp(ch(p.a), 0.0f, 1.0f) : 1.0f);
}
} // namespace

PlyReadResult readPly(std::string_view data, const PlyReadOptions& opts)
{
    LILA_ZONE();

    PlyReadResult r;
    Header h = parseHeader(data);
    if (!h.error.empty())
    {
        r.error = h.error;
        return r;
    }
    r.textureFiles = std::move(h.textureFiles);
    Body body(data.substr(h.bodyStart), h.format);

    const Element* vertexEl = nullptr;
    const Element* faceEl = nullptr;
    const Element* edgeEl = nullptr;
    Columns vertex, face, edge;
    for (const Element& e : h.elements)
    {
        std::vector<bool> want(e.props.size(), false);
        Columns* into = nullptr;
        if (e.name == "vertex" && !vertexEl)
        {
            vertexEl = &e;
            into = &vertex;
            for (size_t i = 0; i < e.props.size(); ++i)
                want[i] = !e.props[i].list;
        }
        else if (e.name == "face" && !faceEl)
        {
            faceEl = &e;
            into = &face;
            for (size_t i = 0; i < e.props.size(); ++i)
            {
                const Property& p = e.props[i];
                want[i] = p.list ? (p.name == "vertex_indices" || p.name == "vertex_index" || p.name == "texcoord") : true;
                if (p.list && !want[i])
                    r.skipped.push_back("face property " + p.name);
            }
        }
        else if (e.name == "edge" && !edgeEl)
        {
            edgeEl = &e;
            into = &edge;
            for (size_t i = 0; i < e.props.size(); ++i)
                want[i] = e.props[i].name == "vertex1" || e.props[i].name == "vertex2";
        }
        else if (e.count > 0)
            r.skipped.push_back("element " + e.name);
        Columns discard;
        if (!readElement(body, e, want, into ? *into : discard))
        {
            r.error = "truncated or malformed data in element \"" + e.name + "\"";
            return r;
        }
    }
    if (!vertexEl)
    {
        r.error = "no vertex element";
        return r;
    }
    const int px = vertexEl->find({"x"}), py = vertexEl->find({"y"}), pz = vertexEl->find({"z"});
    if (px < 0 || py < 0 || pz < 0)
    {
        r.error = "vertex element has no x, y, z";
        return r;
    }

    Mesh& m = r.mesh;
    const size_t nv = vertexEl->count;
    const int fIdx = faceEl ? faceEl->find({"vertex_indices", "vertex_index"}) : -1;
    const size_t nf = fIdx >= 0 ? faceEl->count : 0;
    m.reserve(nv, nf * 2, nf, fIdx >= 0 ? face.listValues[size_t(fIdx)].size() : 0);
    for (size_t k = 0; k < nv; ++k)
        m.addPoint({vertex.scalar[size_t(px)][k], vertex.scalar[size_t(py)][k], vertex.scalar[size_t(pz)][k]});

    // Faces in file order; kept[i] is the file face behind mesh face i.
    std::vector<size_t> kept;
    std::vector<Index> pts;
    if (fIdx >= 0)
    {
        const std::vector<double>& idx = face.listValues[size_t(fIdx)];
        const std::vector<uint32_t>& off = face.listOffsets[size_t(fIdx)];
        size_t bad = 0;
        for (size_t f = 0; f < nf; ++f)
        {
            pts.clear();
            bool ok = true;
            for (uint32_t j = off[f]; j < off[f + 1] && ok; ++j)
            {
                ok = idx[j] >= 0.0 && idx[j] < double(nv);
                pts.push_back(Index(idx[j]));
            }
            if (!ok || m.addFace(pts) == kInvalid)
            {
                ++bad;
                continue;
            }
            kept.push_back(f);
        }
        if (bad)
            r.skipped.push_back(std::to_string(bad) + " bad faces");
    }
    if (edgeEl && edgeEl->find({"vertex1"}) >= 0 && edgeEl->find({"vertex2"}) >= 0)
    {
        const std::vector<double>& a = edge.scalar[size_t(edgeEl->find({"vertex1"}))];
        const std::vector<double>& b = edge.scalar[size_t(edgeEl->find({"vertex2"}))];
        for (size_t k = 0; k < edgeEl->count; ++k)
            if (a[k] >= 0.0 && a[k] < double(nv) && b[k] >= 0.0 && b[k] < double(nv) && a[k] != b[k])
                m.addEdge(Index(a[k]), Index(b[k]));
    }
    r.pointCloud = m.numFaces() == 0 && m.numEdges() == 0;

    const int nx = vertexEl->find({"nx"}), ny = vertexEl->find({"ny"}), nz = vertexEl->find({"nz"});
    const bool hasNormals = opts.keepNormals && nx >= 0 && ny >= 0 && nz >= 0;
    const ColorProps vcol = colorProps(*vertexEl);
    const int ps = vertexEl->find({"s", "u", "texture_u", "texture_s"}), pt = vertexEl->find({"t", "v", "texture_v", "texture_t"});
    const bool hasUv = ps >= 0 && pt >= 0;
    auto normalAt = [&](size_t k) {
        return glm::vec3(float(vertex.scalar[size_t(nx)][k]), float(vertex.scalar[size_t(ny)][k]), float(vertex.scalar[size_t(nz)][k]));
    };
    auto uvAt = [&](size_t k) { return glm::vec2(float(vertex.scalar[size_t(ps)][k]), float(vertex.scalar[size_t(pt)][k])); };

    for (size_t i = 0; i < vertexEl->props.size(); ++i)
    {
        const Property& p = vertexEl->props[i];
        if (p.list)
        {
            r.skipped.push_back("vertex property " + p.name);
            continue;
        }
        if (isBuiltinVertexProperty(p.name))
            continue;
        std::span<float> layer = m.pointAttrs().getOrAdd<float>(extraAttributeName(p.name));
        for (size_t k = 0; k < nv; ++k)
            layer[k] = float(vertex.scalar[i][k]);
    }

    if (m.numFaces() == 0)
    {
        if (hasNormals)
        {
            std::span<glm::vec3> n = m.pointAttrs().getOrAdd<glm::vec3>(attr::kNormal);
            for (size_t k = 0; k < nv; ++k)
                n[k] = normalAt(k);
        }
        if (vcol.any())
        {
            std::span<glm::vec4> c = m.pointAttrs().getOrAdd<glm::vec4>(attr::kColor);
            for (size_t k = 0; k < nv; ++k)
                c[k] = colorAt(*vertexEl, vertex, vcol, k);
        }
        if (hasUv)
        {
            std::span<glm::vec2> uv = m.pointAttrs().getOrAdd<glm::vec2>(attr::uvName(0));
            for (size_t k = 0; k < nv; ++k)
                uv[k] = uvAt(k);
        }
        return r;
    }

    // Faces were added in order without deletions, so face and corner ids are contiguous.
    if (hasNormals)
    {
        std::span<glm::vec3> n = m.cornerAttrs().getOrAdd<glm::vec3>(attr::kNormal);
        for (Index c = 0; c < m.cornerCapacity(); ++c)
            n[c] = normalAt(m.cornerPoint(c));
    }
    const int tc = faceEl->find({"texcoord"});
    if (tc >= 0 && faceEl->props[size_t(tc)].list)
    {
        const std::vector<double>& v = face.listValues[size_t(tc)];
        const std::vector<uint32_t>& off = face.listOffsets[size_t(tc)];
        std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(attr::uvName(0));
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            const size_t src = kept[f];
            const uint32_t count = off[src + 1] - off[src];
            if (count != m.faceSize(f) * 2)
                continue;
            uint32_t j = off[src];
            m.forEachFaceCorner(f, [&](Index c) {
                uv[c] = glm::vec2(float(v[j]), float(v[j + 1]));
                j += 2;
            });
        }
        markUvSeams(m, attr::uvName(0));
    }
    else if (hasUv)
    {
        std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(attr::uvName(0));
        for (Index c = 0; c < m.cornerCapacity(); ++c)
            uv[c] = uvAt(m.cornerPoint(c));
    }
    if (vcol.any())
    {
        std::vector<glm::vec4> perPoint(nv);
        for (size_t k = 0; k < nv; ++k)
            perPoint[k] = colorAt(*vertexEl, vertex, vcol, k);
        colors::add(m, "colorSet");
        colors::setFromPoints(m, "colorSet", perPoint);
    }
    const ColorProps fcol = colorProps(*faceEl);
    if (fcol.any())
    {
        const std::string set = vcol.any() ? "faceColor" : "colorSet";
        std::span<glm::vec4> col = colors::add(m, set);
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            const glm::vec4 c = colorAt(*faceEl, face, fcol, kept[f]);
            m.forEachFaceCorner(f, [&](Index k) { col[k] = c; });
        }
    }
    for (size_t i = 0; i < faceEl->props.size(); ++i)
    {
        const Property& p = faceEl->props[i];
        if (p.list || isBuiltinFaceProperty(p.name))
            continue;
        std::span<float> layer = m.faceAttrs().getOrAdd<float>(extraAttributeName(p.name));
        for (Index f = 0; f < m.faceCapacity(); ++f)
            layer[f] = float(face.scalar[i][kept[f]]);
    }

    // Points no face or edge uses.
    std::vector<Index> loose;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointEdge(p) == kInvalid)
            loose.push_back(p);
    if (!loose.empty())
    {
        m.removeLoose({}, loose);
        m.compact();
    }
    return r;
}

PlyReadResult readPlyFile(const std::string& path, const PlyReadOptions& opts)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        PlyReadResult r;
        r.error = "cannot open " + path;
        return r;
    }
    in.seekg(0, std::ios::end);
    std::string data(size_t(in.tellg()), '\0');
    in.seekg(0);
    in.read(data.data(), std::streamsize(data.size()));
    return readPly(data, opts);
}

namespace
{
struct VertexKey
{
    Index point;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 color;
};

struct VertexKeyHash
{
    size_t operator()(const VertexKey& k) const
    {
        uint64_t h = 1469598103934665603ull;
        const unsigned char* b = reinterpret_cast<const unsigned char*>(&k);
        for (size_t i = 0; i < sizeof(VertexKey); ++i)
            h = (h ^ b[i]) * 1099511628211ull;
        return size_t(h);
    }
};
struct VertexKeyEq
{
    bool operator()(const VertexKey& a, const VertexKey& b) const { return std::memcmp(&a, &b, sizeof(VertexKey)) == 0; }
};

class Out
{
public:
    Out(bool binary, int precision) : mBinary(binary), mPrecision(precision) {}

    std::string text;

    void f32(float v)
    {
        if (mBinary)
            raw(v);
        else
        {
            sep();
            char buf[64];
            auto r = mPrecision > 0 ? std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, mPrecision)
                                    : std::to_chars(buf, buf + sizeof(buf), v);
            text.append(buf, r.ptr);
        }
    }
    template <class T> void integer(T v)
    {
        if (mBinary)
            raw(v);
        else
        {
            sep();
            char buf[24];
            auto r = std::to_chars(buf, buf + sizeof(buf), uint64_t(v));
            text.append(buf, r.ptr);
        }
    }
    void endRow()
    {
        if (!mBinary)
            text += '\n';
        mFirst = true;
    }

private:
    template <class T> void raw(T v)
    {
        char b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));
        if constexpr (std::endian::native == std::endian::big)
            std::reverse(b, b + sizeof(T));
        text.append(b, sizeof(T));
    }
    void sep()
    {
        if (!mFirst)
            text += ' ';
        mFirst = false;
    }

    bool mBinary;
    int mPrecision;
    bool mFirst = true;
};
} // namespace

std::string writePly(const Mesh& mesh, const PlyWriteOptions& opts)
{
    LILA_ZONE();

    std::vector<glm::vec3> computedNormals;
    std::span<const glm::vec3> normals;
    if (opts.normals && mesh.numFaces() > 0)
    {
        normals = mesh.cornerAttrs().get<glm::vec3>(attr::kNormal);
        if (normals.empty())
        {
            computedNormals = cornerNormals(mesh);
            normals = computedNormals;
        }
    }
    std::span<const glm::vec2> uvs = opts.uvs ? mesh.cornerAttrs().get<glm::vec2>(attr::uvName(0)) : std::span<const glm::vec2>();
    const std::string set = opts.colors ? colors::current(mesh) : std::string();
    std::span<const glm::vec4> cols = set.empty() ? std::span<const glm::vec4>() : colors::values(mesh, set);
    // Point clouds carry their attributes per point.
    std::span<const glm::vec3> pointNormals = opts.normals ? mesh.pointAttrs().get<glm::vec3>(attr::kNormal) : std::span<const glm::vec3>();
    std::span<const glm::vec4> pointCols = opts.colors ? mesh.pointAttrs().get<glm::vec4>(attr::kColor) : std::span<const glm::vec4>();
    std::span<const glm::vec2> pointUvs = opts.uvs ? mesh.pointAttrs().get<glm::vec2>(attr::uvName(0)) : std::span<const glm::vec2>();
    const bool writeNormals = !normals.empty() || !pointNormals.empty();
    const bool writeUvs = !uvs.empty() || !pointUvs.empty();
    const bool writeColors = !cols.empty() || !pointCols.empty();

    std::vector<VertexKey> vertices;
    std::unordered_map<VertexKey, uint32_t, VertexKeyHash, VertexKeyEq> ids;
    ids.reserve(mesh.numCorners());
    std::vector<uint32_t> cornerVertex(mesh.cornerCapacity(), 0);
    std::vector<uint32_t> pointVertex(mesh.pointCapacity(), ~0u);
    auto keyFor = [&](Index p, Index c) {
        VertexKey k{};
        k.point = p;
        if (writeNormals)
            k.normal = c != kInvalid && !normals.empty() ? normals[c] : (!pointNormals.empty() ? pointNormals[p] : glm::vec3(0.0f));
        if (writeUvs)
            k.uv = c != kInvalid && !uvs.empty() ? uvs[c] : (!pointUvs.empty() ? pointUvs[p] : glm::vec2(0.0f));
        if (writeColors)
            k.color = c != kInvalid && !cols.empty() ? cols[c] : (!pointCols.empty() ? pointCols[p] : glm::vec4(1.0f));
        return k;
    };
    auto vertexFor = [&](Index p, Index c) {
        auto [it, inserted] = ids.try_emplace(keyFor(p, c), uint32_t(vertices.size()));
        if (inserted)
            vertices.push_back(it->first);
        if (pointVertex[p] == ~0u)
            pointVertex[p] = it->second;
        return it->second;
    };
    size_t faces = 0, maxFace = 0;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        ++faces;
        maxFace = std::max<size_t>(maxFace, mesh.faceSize(f));
        mesh.forEachFaceCorner(f, [&](Index c) { cornerVertex[c] = vertexFor(mesh.cornerPoint(c), c); });
    }
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && pointVertex[p] == ~0u)
            vertexFor(p, kInvalid);
    std::vector<std::pair<uint32_t, uint32_t>> looseEdges;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
        if (mesh.edgeValid(e) && mesh.edgeCorner(e) == kInvalid)
            looseEdges.emplace_back(pointVertex[mesh.edgePoint(e, 0)], pointVertex[mesh.edgePoint(e, 1)]);

    Out out(opts.binary, opts.precision);
    std::string& s = out.text;
    s += "ply\nformat ";
    s += opts.binary ? "binary_little_endian 1.0\n" : "ascii 1.0\n";
    s += "comment Lila\n";
    s += "element vertex " + std::to_string(vertices.size()) + "\n";
    s += "property float x\nproperty float y\nproperty float z\n";
    if (writeNormals)
        s += "property float nx\nproperty float ny\nproperty float nz\n";
    if (writeUvs)
        s += "property float s\nproperty float t\n";
    if (writeColors)
        s += "property uchar red\nproperty uchar green\nproperty uchar blue\nproperty uchar alpha\n";
    const bool wideCount = maxFace > 255;
    if (faces)
    {
        s += "element face " + std::to_string(faces) + "\n";
        s += wideCount ? "property list int int vertex_indices\n" : "property list uchar int vertex_indices\n";
    }
    if (!looseEdges.empty())
        s += "element edge " + std::to_string(looseEdges.size()) + "\nproperty int vertex1\nproperty int vertex2\n";
    s += "end_header\n";

    auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (const VertexKey& v : vertices)
    {
        const glm::dvec3& p = mesh.position(v.point);
        for (int i = 0; i < 3; ++i)
            out.f32(float(p[i]));
        if (writeNormals)
            for (int i = 0; i < 3; ++i)
                out.f32(v.normal[i]);
        if (writeUvs)
        {
            out.f32(v.uv.x);
            out.f32(v.uv.y);
        }
        if (writeColors)
        {
            for (int i = 0; i < 3; ++i)
                out.integer(byte(linearToSrgb(v.color[i])));
            out.integer(byte(v.color.a));
        }
        out.endRow();
    }
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        if (wideCount)
            out.integer(int32_t(mesh.faceSize(f)));
        else
            out.integer(uint8_t(mesh.faceSize(f)));
        mesh.forEachFaceCorner(f, [&](Index c) { out.integer(int32_t(cornerVertex[c])); });
        out.endRow();
    }
    for (auto [a, b] : looseEdges)
    {
        out.integer(int32_t(a));
        out.integer(int32_t(b));
        out.endRow();
    }
    return s;
}
} // namespace lila::geo
