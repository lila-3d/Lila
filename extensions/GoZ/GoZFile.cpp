#include "GoZFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace lila;

namespace goz
{
namespace
{
enum Tag : uint32_t
{
    kEnd = 0,
    kName = 1,
    kFlags = 0x1389,
    kPoints = 0x2711,
    kFaces = 0x4e21,
    kUvs = 0x61a9,
    kMask = 0x7532,
    kPolypaint = 0x88b9,
    kGroups = 0x9c41,
};

constexpr std::string_view kHeader = "GoZb 1.0 ZBrush GoZ Binary......";
constexpr std::string_view kMeshPrefix = "GoZMesh_";
constexpr uint32_t kNoCorner = 0xffffffffu;

float toSrgb(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

float toLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

struct Writer
{
    std::string out;

    template <class T> void put(const T& v) { out.append(reinterpret_cast<const char*>(&v), sizeof(T)); }

    /** Chunk sizes count the 16-byte tag, size and count header. */
    void chunk(uint32_t tag, uint64_t count, size_t itemBytes)
    {
        put(tag);
        put(uint32_t(16 + count * itemBytes));
        put(count);
    }
};

class Reader
{
public:
    explicit Reader(std::span<const char> bytes) : mBytes(bytes) {}

    template <class T> T at(size_t pos) const
    {
        if (pos + sizeof(T) > mBytes.size())
            throw std::runtime_error("the GoZ file is cut short");
        T v;
        std::memcpy(&v, mBytes.data() + pos, sizeof(T));
        return v;
    }

    /** A chunk's item count, checked against the bytes left. */
    uint64_t count(size_t pos, size_t itemBytes) const
    {
        const uint64_t n = at<uint64_t>(pos + 8);
        if (n > (mBytes.size() - pos - 16) / itemBytes)
            throw std::runtime_error("the GoZ file is cut short");
        return n;
    }

    size_t size() const { return mBytes.size(); }
    const char* data(size_t pos) const { return mBytes.data() + pos; }

private:
    std::span<const char> mBytes;
};

bool validFace(std::span<const uint32_t> pts, size_t numPoints)
{
    for (size_t i = 0; i < pts.size(); ++i)
    {
        if (pts[i] >= numPoints)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (pts[i] == pts[j])
                return false;
    }
    return true;
}
} // namespace

std::string write(const geo::Mesh& mesh, std::string_view name)
{
    std::vector<uint32_t> dense(mesh.pointCapacity(), kNoCorner);
    std::vector<geo::Index> points;
    points.reserve(mesh.numPoints());
    for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
        {
            dense[p] = uint32_t(points.size());
            points.push_back(p);
        }

    // GoZ faces are triangles and quads: larger faces fan from their first corner.
    struct OutFace
    {
        geo::Index face;
        std::array<geo::Index, 4> corners;
        int size;
    };
    std::vector<OutFace> faces;
    faces.reserve(mesh.numFaces());
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const std::vector<geo::Index> c = mesh.faceCorners(f);
        if (c.size() == 3 || c.size() == 4)
            faces.push_back({f, {c[0], c[1], c[2], c.size() == 4 ? c[3] : geo::kInvalid}, int(c.size())});
        else
            for (size_t k = 1; k + 1 < c.size(); ++k)
                faces.push_back({f, {c[0], c[k], c[k + 1], geo::kInvalid}, 3});
    }

    Writer w;
    w.out.reserve(64 + points.size() * 16 + faces.size() * 52);
    w.out.append(kHeader);

    std::string label = std::string(kMeshPrefix) + std::string(name);
    label.push_back('\0');
    w.chunk(kName, 1, label.size());
    w.out.append(label);

    w.chunk(kFlags, 1, 4);
    w.put(uint32_t(0));

    w.chunk(kPoints, points.size(), 12);
    for (geo::Index p : points)
    {
        const glm::vec3 v(mesh.position(p));
        w.put(v);
    }

    w.chunk(kFaces, faces.size(), 16);
    for (const OutFace& f : faces)
        for (int k = 0; k < 4; ++k)
            w.put(k < f.size ? dense[mesh.cornerPoint(f.corners[k])] : kNoCorner);

    if (std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)); !uv.empty())
    {
        w.chunk(kUvs, faces.size(), 32);
        for (const OutFace& f : faces)
            for (int k = 0; k < 4; ++k)
            {
                const glm::vec2 t = k < f.size ? uv[f.corners[k]] : glm::vec2(0.0f, 0.0f);
                w.put(glm::vec2(t.x, 1.0f - t.y));
            }
    }

    if (const std::string set = geo::colors::current(mesh); !set.empty())
    {
        const std::vector<glm::vec4> colors = geo::colors::pointColors(mesh, set);
        w.chunk(kPolypaint, points.size(), 4);
        for (geo::Index p : points)
        {
            const glm::vec4& c = colors[p];
            auto byte = [](float v) { return uint8_t(std::lround(toSrgb(v) * 255.0f)); };
            w.put(std::array<uint8_t, 4>{byte(c.b), byte(c.g), byte(c.r), 0xff});
        }
    }

    if (std::span<const float> mask = mesh.pointAttrs().get<float>(kMaskLayer); !mask.empty())
    {
        w.chunk(kMask, points.size(), 2);
        for (geo::Index p : points)
            w.put(uint16_t(std::lround((1.0f - std::clamp(mask[p], 0.0f, 1.0f)) * 65535.0f)));
    }

    if (std::span<const int32_t> groups = mesh.faceAttrs().get<int32_t>(kPolygroupLayer); !groups.empty())
    {
        w.chunk(kGroups, faces.size(), 2);
        for (const OutFace& f : faces)
            w.put(uint16_t(std::clamp(groups[f.face], 0, 0xffff)));
    }

    w.out.append(16, '\0');
    return std::move(w.out);
}

File read(std::span<const char> bytes)
{
    if (bytes.size() < kHeader.size() || std::string_view(bytes.data(), 4) != "GoZb")
        throw std::runtime_error("not a GoZ file");
    const Reader r(bytes);

    File out;
    std::vector<glm::dvec3> points;
    std::vector<uint32_t> faces; // 4 per face, the fourth kNoCorner on triangles
    std::vector<glm::vec2> uvs;  // 4 per face
    std::vector<glm::vec4> paint;
    std::vector<float> mask;
    std::vector<int32_t> groups;

    size_t pos = kHeader.size();
    while (pos + 4 <= r.size())
    {
        const uint32_t tag = r.at<uint32_t>(pos);
        if (tag == kEnd)
            break;
        const uint32_t chunkSize = r.at<uint32_t>(pos + 4);
        size_t next = pos + chunkSize;
        switch (tag)
        {
        case kName:
        {
            const size_t len = chunkSize >= 16 ? std::min<size_t>(chunkSize - 16, r.size() - pos - 16) : 0;
            std::string_view s(r.data(pos + 16), len);
            s = s.substr(0, s.find('\0'));
            if (s.starts_with(kMeshPrefix))
                s.remove_prefix(kMeshPrefix.size());
            out.name = s;
            break;
        }
        case kPoints:
        {
            const uint64_t n = r.count(pos, 12);
            points.resize(n);
            for (uint64_t i = 0; i < n; ++i)
                points[i] = glm::dvec3(r.at<glm::vec3>(pos + 16 + i * 12));
            next = pos + 16 + n * 12;
            break;
        }
        case kFaces:
        {
            const uint64_t n = r.count(pos, 16);
            faces.resize(n * 4);
            std::memcpy(faces.data(), r.data(pos + 16), n * 16);
            next = pos + 16 + n * 16;
            break;
        }
        case kUvs:
        {
            const uint64_t n = r.count(pos, 32);
            uvs.resize(n * 4);
            for (uint64_t i = 0; i < n * 4; ++i)
            {
                const glm::vec2 t = r.at<glm::vec2>(pos + 16 + i * 8);
                uvs[i] = glm::vec2(t.x, 1.0f - t.y);
            }
            next = pos + 16 + n * 32;
            break;
        }
        case kPolypaint:
        {
            const uint64_t n = r.count(pos, 4);
            paint.resize(n);
            for (uint64_t i = 0; i < n; ++i)
            {
                const auto c = r.at<std::array<uint8_t, 4>>(pos + 16 + i * 4);
                paint[i] = glm::vec4(toLinear(c[2] / 255.0f), toLinear(c[1] / 255.0f), toLinear(c[0] / 255.0f), 1.0f);
            }
            next = pos + 16 + n * 4;
            break;
        }
        case kMask:
        {
            const uint64_t n = r.count(pos, 2);
            mask.resize(n);
            for (uint64_t i = 0; i < n; ++i)
                mask[i] = 1.0f - r.at<uint16_t>(pos + 16 + i * 2) / 65535.0f;
            next = pos + 16 + n * 2;
            break;
        }
        case kGroups:
        {
            const uint64_t n = r.count(pos, 2);
            groups.resize(n);
            for (uint64_t i = 0; i < n; ++i)
                groups[i] = r.at<uint16_t>(pos + 16 + i * 2);
            next = pos + 16 + n * 2;
            break;
        }
        default:
            if (chunkSize < 8)
                throw std::runtime_error("the GoZ file has a broken chunk");
            break;
        }
        pos = next;
    }
    if (points.empty())
        throw std::runtime_error("the GoZ file has no points");

    const size_t numFaces = faces.size() / 4;
    std::vector<geo::Index> sizes, indices;
    std::vector<size_t> kept; // source face of each face handed to the mesh
    sizes.reserve(numFaces);
    indices.reserve(faces.size());
    kept.reserve(numFaces);
    for (size_t f = 0; f < numFaces; ++f)
    {
        const std::span<const uint32_t> pts(faces.data() + f * 4, faces[f * 4 + 3] == kNoCorner ? 3 : 4);
        if (!validFace(pts, points.size()))
            continue;
        sizes.push_back(geo::Index(pts.size()));
        indices.insert(indices.end(), pts.begin(), pts.end());
        kept.push_back(f);
    }
    out.mesh = geo::Mesh::fromPolygons(points, sizes, indices);
    geo::Mesh& m = out.mesh;

    // Per-face data needs every kept face to have made it into the mesh, in order.
    if (m.numFaces() == kept.size() && m.faceCapacity() == kept.size())
    {
        if (uvs.size() == faces.size())
        {
            std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
            for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            {
                geo::Index c = m.faceCorner(f);
                for (geo::Index k = 0; k < m.faceSize(f); ++k, c = m.cornerNext(c))
                    uv[c] = uvs[kept[f] * 4 + k];
            }
        }
        if (groups.size() == numFaces)
        {
            std::span<int32_t> g = m.faceAttrs().getOrAdd<int32_t>(kPolygroupLayer);
            for (geo::Index f = 0; f < m.faceCapacity(); ++f)
                g[f] = groups[kept[f]];
        }
    }
    if (mask.size() == points.size())
        std::ranges::copy(mask, m.pointAttrs().getOrAdd<float>(kMaskLayer).begin());
    if (paint.size() == points.size())
    {
        geo::colors::add(m, kPolypaintSet);
        geo::colors::setFromPoints(m, kPolypaintSet, paint);
    }
    return out;
}
} // namespace goz
