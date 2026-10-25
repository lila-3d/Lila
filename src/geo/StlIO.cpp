#include "geo/StlIO.h"

#include "geo/ColorSets.h"
#include "geo/Normals.h"
#include "geo/Triangulate.h"

#include "core/Profile.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace lila::geo
{
namespace
{
constexpr size_t kHeader = 80;
constexpr size_t kTriangle = 50;

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

template <class T> T load(const char* p)
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

struct PositionHash
{
    size_t operator()(const glm::vec3& v) const
    {
        uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < 3; ++i)
            h = (h ^ std::bit_cast<uint32_t>(v[i])) * 1099511628211ull;
        return size_t(h);
    }
};

/** Collects triangles (or ASCII polygons) and builds the mesh, welding identical positions. */
struct Builder
{
    const StlReadOptions& opts;
    Mesh mesh;
    std::unordered_map<glm::vec3, Index, PositionHash> points;
    std::vector<Index> facePoints;
    std::vector<glm::vec4> faceColor;
    bool anyColor = false;

    Index point(glm::vec3 p)
    {
        for (int i = 0; i < 3; ++i)
            if (p[i] == 0.0f)
                p[i] = 0.0f; // -0 and +0 weld
        if (!opts.weld)
            return mesh.addPoint(glm::dvec3(p));
        auto [it, inserted] = points.try_emplace(p, kInvalid);
        if (inserted)
            it->second = mesh.addPoint(glm::dvec3(p));
        return it->second;
    }

    void face(std::span<const glm::vec3> corners, const glm::vec4* color)
    {
        facePoints.clear();
        for (const glm::vec3& c : corners)
            facePoints.push_back(point(c));
        if (mesh.addFace(facePoints) == kInvalid)
            return;
        faceColor.push_back(color ? *color : glm::vec4(1.0f));
        anyColor |= color != nullptr;
    }

    Mesh finish()
    {
        mesh.removeAllLoose();
        if (mesh.hasDeleted())
            mesh.compact();
        if (anyColor)
        {
            std::span<glm::vec4> col = colors::add(mesh, "colorSet");
            for (Index f = 0; f < mesh.faceCapacity(); ++f)
                mesh.forEachFaceCorner(f, [&](Index c) { col[c] = faceColor[f]; });
        }
        if (opts.hardAngle >= 0.0)
            setHardEdgesByAngle(mesh, opts.hardAngle);
        return std::move(mesh);
    }
};

/** VisCAM sets bit 15 on colored triangles with red high; Materialise ("COLOR=" header) clears it with red low. */
bool triangleColor(uint16_t attr, bool materialise, glm::vec4& out)
{
    const bool has = materialise ? (attr & 0x8000) == 0 : (attr & 0x8000) != 0;
    if (!has)
        return false;
    const float lo = float(attr & 31) / 31.0f, mid = float((attr >> 5) & 31) / 31.0f, hi = float((attr >> 10) & 31) / 31.0f;
    const float r = materialise ? lo : hi, b = materialise ? hi : lo;
    out = glm::vec4(srgbToLinear(r), srgbToLinear(mid), srgbToLinear(b), 1.0f);
    return true;
}

void readBinary(std::string_view data, const StlReadOptions& opts, StlReadResult& r)
{
    const uint32_t n = load<uint32_t>(data.data() + kHeader);
    const std::string_view header = data.substr(0, kHeader);
    const bool materialise = header.find("COLOR=") != std::string_view::npos;
    Builder b{opts};
    b.mesh.reserve(opts.weld ? n / 2 + 3 : size_t(n) * 3, size_t(n) * 3 / 2, n, size_t(n) * 3);
    if (opts.weld)
        b.points.reserve(n / 2 + 3);
    // Most writers leave the attribute bytes zero. Trust them as colors when they vary or all carry the VisCAM flag.
    uint16_t firstAttr = 0;
    bool attrsDiffer = false, allFlagged = n > 0;
    for (uint32_t t = 0; t < n; ++t)
    {
        const uint16_t attr = load<uint16_t>(data.data() + kHeader + 4 + t * kTriangle + 48);
        if (t == 0)
            firstAttr = attr;
        attrsDiffer |= attr != firstAttr;
        allFlagged &= (attr & 0x8000) != 0;
    }
    const bool useColors = attrsDiffer || allFlagged || (materialise && n > 0);
    glm::vec3 corners[3];
    for (uint32_t t = 0; t < n; ++t)
    {
        const char* p = data.data() + kHeader + 4 + t * kTriangle;
        for (int k = 0; k < 3; ++k)
            corners[k] = glm::vec3(load<float>(p + 12 + k * 12), load<float>(p + 16 + k * 12), load<float>(p + 20 + k * 12));
        glm::vec4 color;
        const bool has = useColors && triangleColor(load<uint16_t>(p + 48), materialise, color);
        b.face(corners, has ? &color : nullptr);
    }
    r.binary = true;
    r.colors = b.anyColor;
    StlSolid& s = r.solids.emplace_back();
    std::string_view name = header.substr(0, header.find('\0'));
    if (name.starts_with("solid "))
        name.remove_prefix(6);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
        name.remove_suffix(1);
    s.name = std::string(name);
    s.mesh = b.finish();
}

struct Tokens
{
    std::string_view text;
    size_t at = 0;

    std::string_view next()
    {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
            ++at;
        const size_t start = at;
        while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at])))
            ++at;
        return text.substr(start, at - start);
    }
    /** The rest of the line, trimmed. */
    std::string_view line()
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t'))
            ++at;
        const size_t start = at;
        while (at < text.size() && text[at] != '\n' && text[at] != '\r')
            ++at;
        std::string_view s = text.substr(start, at - start);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.remove_suffix(1);
        return s;
    }
    bool number(float& out)
    {
        std::string_view t = next();
        if (!t.empty() && t[0] == '+')
            t.remove_prefix(1);
        auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), out);
        return ec == std::errc() && ptr == t.data() + t.size();
    }
};

void readAscii(std::string_view data, const StlReadOptions& opts, StlReadResult& r)
{
    Tokens tok{data};
    std::vector<glm::vec3> loop;
    while (true)
    {
        std::string_view t = tok.next();
        if (t.empty())
            break;
        if (t != "solid")
        {
            r.error = "expected \"solid\", found \"" + std::string(t.substr(0, 32)) + "\"";
            return;
        }
        StlSolid& s = r.solids.emplace_back();
        s.name = std::string(tok.line());
        Builder b{opts};
        bool closed = false;
        while (!closed)
        {
            t = tok.next();
            if (t.empty())
                break;
            if (t == "endsolid")
            {
                tok.line();
                closed = true;
            }
            else if (t == "vertex")
            {
                glm::vec3 v;
                if (!tok.number(v.x) || !tok.number(v.y) || !tok.number(v.z))
                {
                    r.error = "bad vertex in solid \"" + s.name + "\"";
                    return;
                }
                loop.push_back(v);
            }
            else if (t == "endloop")
            {
                if (loop.size() >= 3)
                    b.face(loop, nullptr);
                loop.clear();
            }
            // facet normal, outer loop and endfacet carry nothing the mesh needs: normals are recomputed.
        }
        s.mesh = b.finish();
    }
    if (r.solids.empty())
        r.error = "no solids";
}

std::string_view trimLeft(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    return s;
}

void appendNumber(std::string& s, float v, int precision)
{
    char buf[64];
    auto r = precision > 0 ? std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, precision)
                           : std::to_chars(buf, buf + sizeof(buf), v);
    s.append(buf, r.ptr);
}

template <class T> void store(std::string& s, T v)
{
    char b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    s.append(b, sizeof(T));
}

struct Triangle
{
    glm::vec3 p[3];
    glm::vec3 normal;
    uint16_t attr = 0;
};

std::vector<Triangle> triangles(const Mesh& m, bool wantColors)
{
    const Triangulation tris = triangulate(m);
    const std::string set = wantColors ? colors::current(m) : std::string();
    std::span<const glm::vec4> col = set.empty() ? std::span<const glm::vec4>() : colors::values(m, set);
    std::vector<Triangle> out(tris.triangleCount());
    for (Index t = 0; t < tris.triangleCount(); ++t)
    {
        Triangle& tri = out[t];
        glm::vec4 sum(0.0f);
        for (int k = 0; k < 3; ++k)
        {
            const Index c = tris.corners[t * 3 + k];
            tri.p[k] = glm::vec3(m.position(m.cornerPoint(c)));
            if (!col.empty())
                sum += col[c];
        }
        const glm::dvec3 n = glm::cross(m.position(m.cornerPoint(tris.corners[t * 3 + 1])) - m.position(m.cornerPoint(tris.corners[t * 3])),
                                        m.position(m.cornerPoint(tris.corners[t * 3 + 2])) - m.position(m.cornerPoint(tris.corners[t * 3])));
        const double len = glm::length(n);
        tri.normal = len > 0.0 ? glm::vec3(n / len) : glm::vec3(0.0f);
        if (!col.empty())
        {
            auto q = [](float v) { return uint16_t(std::lround(linearToSrgb(v) * 31.0f)); };
            sum /= 3.0f;
            tri.attr = uint16_t(0x8000 | (q(sum.r) << 10) | (q(sum.g) << 5) | q(sum.b));
        }
    }
    return out;
}
} // namespace

StlReadResult readStl(std::string_view data, const StlReadOptions& opts)
{
    LILA_ZONE();

    StlReadResult r;
    const bool sizedBinary = data.size() >= kHeader + 4 && kHeader + 4 + size_t(load<uint32_t>(data.data() + kHeader)) * kTriangle == data.size();
    if (sizedBinary)
        readBinary(data, opts, r);
    else if (trimLeft(data).starts_with("solid"))
        readAscii(data, opts, r);
    else if (data.size() >= kHeader + 4 && kHeader + 4 + size_t(load<uint32_t>(data.data() + kHeader)) * kTriangle <= data.size())
        readBinary(data, opts, r); // binary with trailing bytes
    else
        r.error = data.size() < kHeader + 4 ? "file too short" : "not an STL file, or truncated";
    return r;
}

StlReadResult readStlFile(const std::string& path, const StlReadOptions& opts)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        StlReadResult r;
        r.error = "cannot open " + path;
        return r;
    }
    in.seekg(0, std::ios::end);
    std::string data(size_t(in.tellg()), '\0');
    in.seekg(0);
    in.read(data.data(), std::streamsize(data.size()));
    return readStl(data, opts);
}

std::string writeStl(std::span<const StlSolid> solids, const StlWriteOptions& opts)
{
    LILA_ZONE();

    std::string s;
    if (opts.binary)
    {
        std::vector<Triangle> all;
        for (const StlSolid& solid : solids)
        {
            std::vector<Triangle> t = triangles(solid.mesh, opts.colors);
            all.insert(all.end(), t.begin(), t.end());
        }
        s.reserve(kHeader + 4 + all.size() * kTriangle);
        // Never "solid" first, or readers that sniff the header take it for ASCII.
        std::string header = solids.size() == 1 && !solids[0].name.empty() ? "Lila STL: " + solids[0].name : std::string("Lila STL");
        header.resize(kHeader, '\0');
        s += header;
        store(s, uint32_t(all.size()));
        for (const Triangle& t : all)
        {
            for (int i = 0; i < 3; ++i)
                store(s, t.normal[i]);
            for (const glm::vec3& p : t.p)
                for (int i = 0; i < 3; ++i)
                    store(s, p[i]);
            store(s, t.attr);
        }
        return s;
    }
    for (const StlSolid& solid : solids)
    {
        const std::string name = solid.name.empty() ? "mesh" : solid.name;
        s += "solid " + name + "\n";
        for (const Triangle& t : triangles(solid.mesh, false))
        {
            s += "  facet normal ";
            for (int i = 0; i < 3; ++i)
            {
                appendNumber(s, t.normal[i], opts.precision);
                s += i < 2 ? ' ' : '\n';
            }
            s += "    outer loop\n";
            for (const glm::vec3& p : t.p)
            {
                s += "      vertex ";
                for (int i = 0; i < 3; ++i)
                {
                    appendNumber(s, p[i], opts.precision);
                    s += i < 2 ? ' ' : '\n';
                }
            }
            s += "    endloop\n  endfacet\n";
        }
        s += "endsolid " + name + "\n";
    }
    return s;
}
} // namespace lila::geo
