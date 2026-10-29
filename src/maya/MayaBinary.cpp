#include "maya/MayaFile.h"

#include "core/Profile.h"
#include "core/Progress.h"

#include <bit>
#include <cstring>
#include <format>
#include <set>
#include <unordered_map>

/**
 * Maya binary files are IFF. Every chunk is a 4-byte tag and a big-endian size: in "FOR8" files a 4-byte field and
 * an 8-byte size, in older "FOR4" files a 4-byte size. Group chunks (FOR*, LIS*, CAT*) start with a 4-byte form type
 * and hold child chunks. Leaf data is padded to 8 bytes in FOR8 files (4 in FOR4); groups to 4.
 *
 *   FOR8 Maya
 *     FOR8 HEAD   VERS, LUNI/AUNI/TUNI (units), TDUR (ticks per second), FINF (fileInfo), PLUG (requires)
 *     FOR8 FREF   file references
 *     FOR8 <node type id>   one per createNode: CREA (flags, name, parent, uuid) then one chunk per setAttr
 *     FOR8 SLCT   select -ne: SLCT (name) then setAttr chunks
 *     LIS8 CONS / FOR8 CONN   CWFL (connectAttr) and RELA (relationship)
 *
 * A setAttr chunk's tag is the value's type (DBLE, DBL3, FLT2, STR , CMP#, MATR, MESH, ...) and its data is the
 * attribute path, a flag byte (0x08: a 32-bit -s size follows), then the values. Values are in internal units.
 */
namespace lila::maya
{
namespace
{
constexpr uint32_t tag(const char (&s)[5])
{
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

class Cursor
{
public:
    Cursor(const uint8_t* p, size_t n) : mP(p), mN(n) {}

    bool ok() const { return mOk; }
    size_t left() const { return mPos <= mN ? mN - mPos : 0; }
    size_t pos() const { return mPos; }

    uint8_t u8()
    {
        if (!need(1))
            return 0;
        return mP[mPos++];
    }
    uint32_t u32()
    {
        if (!need(4))
            return 0;
        uint32_t v = (uint32_t(mP[mPos]) << 24) | (uint32_t(mP[mPos + 1]) << 16) | (uint32_t(mP[mPos + 2]) << 8) | uint32_t(mP[mPos + 3]);
        mPos += 4;
        return v;
    }
    uint64_t u64()
    {
        uint64_t hi = u32();
        return (hi << 32) | u32();
    }
    float f32() { return std::bit_cast<float>(u32()); }
    double f64() { return std::bit_cast<double>(u64()); }
    std::string str()
    {
        size_t start = mPos;
        while (mPos < mN && mP[mPos] != 0)
            ++mPos;
        std::string s(reinterpret_cast<const char*>(mP + start), mPos - start);
        if (mPos < mN)
            ++mPos;
        else
            mOk = false;
        return s;
    }
    /** A count that must leave room for `each` bytes per item. */
    uint32_t count(size_t each)
    {
        uint32_t n = u32();
        if (each && size_t(n) > left() / each)
        {
            mOk = false;
            return 0;
        }
        return n;
    }
    void skip(size_t n)
    {
        if (need(n))
            mPos += n;
    }

private:
    bool need(size_t n)
    {
        if (!mOk || mPos + n > mN)
        {
            mOk = false;
            return false;
        }
        return true;
    }

    const uint8_t* mP;
    size_t mN;
    size_t mPos = 0;
    bool mOk = true;
};

struct Chunk
{
    uint32_t tag = 0;
    uint32_t form = 0; // groups only
    const uint8_t* data = nullptr; // groups: after the form type
    size_t size = 0;
    bool group = false;
    bool wide = true;
};

bool isGroup(uint32_t t)
{
    return t == tag("FOR4") || t == tag("FOR8") || t == tag("LIS4") || t == tag("LIS8") || t == tag("CAT4") || t == tag("CAT8") ||
           t == tag("PROP") || t == tag("PRO8");
}

/** Children of a group's data. Stops quietly at a truncated chunk. */
std::vector<Chunk> children(const uint8_t* p, size_t n, bool wide, bool* truncated)
{
    std::vector<Chunk> out;
    const size_t header = wide ? 16 : 8;
    size_t pos = 0;
    while (pos + header <= n)
    {
        Chunk c;
        Cursor cur(p + pos, header);
        c.tag = cur.u32();
        uint64_t size = 0;
        if (wide)
        {
            cur.u32();
            size = cur.u64();
        }
        else
            size = cur.u32();
        size_t body = pos + header;
        if (size > n - body)
        {
            *truncated = true;
            break;
        }
        c.group = isGroup(c.tag);
        if (c.group)
        {
            if (size < 4)
                break;
            c.wide = (c.tag & 0xFF) == '8' || c.tag == tag("PRO8");
            c.form = Cursor(p + body, 4).u32();
            c.data = p + body + 4;
            c.size = size_t(size) - 4;
            pos = body + ((size_t(size) + 3) & ~size_t(3));
        }
        else
        {
            c.wide = wide;
            c.data = p + body;
            c.size = size_t(size);
            const size_t align = wide ? 8 : 4;
            pos = body + ((size_t(size) + align - 1) / align * align);
        }
        out.push_back(c);
    }
    return out;
}

std::string tagName(uint32_t t)
{
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i)
    {
        char c = char((t >> (24 - 8 * i)) & 0xFF);
        s[size_t(i)] = c >= 32 && c < 127 ? c : '?';
    }
    return s;
}

std::vector<std::string> nulStrings(const Chunk& c)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < c.size; ++i)
    {
        char ch = char(c.data[i]);
        if (ch == 0)
        {
            out.push_back(cur);
            cur.clear();
        }
        else
            cur.push_back(ch);
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

/** "wl[0:19].w": a ranged multi whose elements are sparse multis, written as count, (index, value) pairs. */
bool sparsePath(const std::vector<AttrSeg>& segs)
{
    for (size_t i = 0; i + 1 < segs.size(); ++i)
        if (segs[i].lo >= 0 && segs[i].hi > segs[i].lo && segs.back().lo == -1)
            return true;
    return false;
}

std::shared_ptr<MeshData> decodeMesh(Cursor& c, std::string* warning)
{
    auto m = std::make_shared<MeshData>();
    uint32_t nf = c.count(4);
    m->points.resize(nf / 3);
    for (auto& p : m->points)
        for (int k = 0; k < 3; ++k)
            p[k] = c.f32();
    c.skip(4 * (nf % 3));

    uint32_t ne = c.count(4);
    m->edges.resize(ne / 2);
    m->edgeHard.resize(ne / 2);
    for (size_t e = 0; e < m->edges.size(); ++e)
    {
        uint32_t a = c.u32(), b = c.u32();
        m->edgeHard[e] = (a & 0x80000000u) ? 1 : 0;
        m->edges[e] = {int32_t(a & 0x7FFFFFFFu), int32_t(b & 0x7FFFFFFFu)};
    }

    uint32_t nfe = c.count(4);
    size_t faceVertices = nfe;
    MeshData::Face face;
    MeshData::Loop loop;
    for (uint32_t i = 0; i < nfe; ++i)
    {
        uint32_t w = c.u32();
        int32_t e = int32_t(w & 0x1FFFFFFFu);
        loop.edges.push_back((w & 0x80000000u) ? ~e : e);
        if (w & 0x40000000u)
        {
            face.loops.push_back(std::move(loop));
            loop = {};
        }
        if (w & 0x20000000u)
        {
            if (!loop.edges.empty())
                face.loops.push_back(std::move(loop));
            loop = {};
            m->faces.push_back(std::move(face));
            face = {};
        }
    }
    if (!c.ok())
    {
        *warning = "mesh data is truncated";
        return m;
    }

    uint32_t nn = c.count(4);
    m->normals.resize(nn / 3);
    for (auto& n : m->normals)
        for (int k = 0; k < 3; ++k)
            n[k] = c.f32();
    c.skip(4 * (nn % 3));
    if (nn > 0)
        c.skip(4 * size_t(c.count(4)));

    // Per-face-vertex index lists are flat over every loop, in face order.
    auto distribute = [&](const std::vector<int32_t>& flat, bool uv, size_t set) {
        if (flat.size() != faceVertices)
            return false;
        size_t k = 0;
        for (auto& f : m->faces)
            for (auto& l : f.loops)
            {
                auto& slots = uv ? l.uv : l.color;
                if (slots.size() <= set)
                    slots.resize(set + 1);
                slots[set].assign(flat.begin() + ptrdiff_t(k), flat.begin() + ptrdiff_t(k + l.edges.size()));
                k += l.edges.size();
            }
        return true;
    };

    uint32_t nsets = c.count(1);
    if (nsets > 0)
    {
        c.u32(); // current uv set
        for (uint32_t s = 0; s < nsets && c.ok(); ++s)
        {
            MeshData::UvSet set;
            set.name = c.str();
            uint32_t n = c.count(4);
            set.uvs.resize(n / 2);
            for (auto& uv : set.uvs)
                for (int k = 0; k < 2; ++k)
                    uv[k] = c.f32();
            c.skip(4 * (n % 2));
            uint32_t nfv = c.count(4);
            std::vector<int32_t> idx(nfv);
            for (auto& i : idx)
                i = int32_t(c.u32());
            m->uvSets.push_back(std::move(set));
            if (!distribute(idx, true, s))
                *warning = std::format("uv set '{}' has {} face-vertex entries for {}; left out", m->uvSets.back().name, nfv, faceVertices);
        }
    }

    uint32_t ncol = c.count(1);
    if (ncol > 0 && c.ok())
    {
        c.u32(); // current color set
        for (uint32_t s = 0; s < ncol && c.ok(); ++s)
        {
            MeshData::ColorSet set;
            set.name = c.str();
            uint32_t rep = c.u32();
            set.channels = rep == 2 ? 1 : rep == 3 ? 3 : 4;
            uint32_t n = c.count(4);
            set.colors.resize(n / 4);
            for (auto& col : set.colors)
                for (int k = 0; k < 4; ++k)
                    col[k] = c.f32();
            c.skip(4 * (n % 4));
            uint32_t nfv = c.count(4);
            std::vector<int32_t> idx(nfv);
            for (auto& i : idx)
                i = int32_t(c.u32());
            m->colorSets.push_back(std::move(set));
            if (!distribute(idx, false, s))
                *warning = std::format("color set '{}' does not match the faces; left out", m->colorSets.back().name);
        }
    }

    // Edge creases, then vertex creases: present flag, then an Index_Data block of (index, value count, values).
    for (auto* out : {&m->edgeCreases, &m->vertexCreases})
    {
        if (!c.ok() || c.left() < 4)
            break;
        if (c.u32() == 0)
            continue;
        c.u32();
        c.u32(); // component kind
        uint32_t n = c.count(8);
        for (uint32_t i = 0; i < n && c.ok(); ++i)
        {
            int32_t index = int32_t(c.u32());
            uint32_t k = c.count(4);
            float v = 0.0f;
            for (uint32_t j = 0; j < k; ++j)
                v = j == 0 ? c.f32() : (c.f32(), v);
            out->push_back({index, v});
        }
    }
    if (!c.ok())
    {
        m->edgeCreases.clear();
        m->vertexCreases.clear();
    }
    return m;
}

std::string componentKind(uint32_t t)
{
    switch (t)
    {
    case tag("CMDF"): return "f";
    case tag("CMDE"): return "e";
    case tag("CMDV"): return "vtx";
    case tag("CMDM"): return "map";
    default: return tagName(t);
    }
}

class Decoder
{
public:
    File f;

    void run(std::span<const uint8_t> bytes)
    {
        bool truncated = false;
        std::vector<Chunk> top = children(bytes.data(), bytes.size(), Cursor(bytes.data(), 4).u32() == tag("FOR8"), &truncated);
        if (top.empty() || !top[0].group || top[0].form != tag("Maya"))
        {
            f.warnings.push_back("not a Maya binary file");
            return;
        }
        progress::Scope scope("Parsing", double(bytes.size()));
        for (const Chunk& c : children(top[0].data, top[0].size, top[0].wide, &truncated))
        {
            topLevel(c);
            scope.set(double(c.data - bytes.data()));
            scope.checkCancel();
        }
        for (Reference& r : f.references)
            r.loaded = !mUnloaded.contains(r.refNode);
        if (truncated)
            f.warnings.push_back("the file is truncated; what could be read was kept");
        for (const auto& [code, n] : mUnknown)
            f.warnings.push_back(std::format("{} node(s) of unknown binary type id {:08X} skipped", n, code));
    }

private:
    void topLevel(const Chunk& c)
    {
        bool truncated = false;
        if (!c.group)
            return;
        auto kids = children(c.data, c.size, c.wide, &truncated);
        if (c.form == tag("HEAD"))
            header(kids);
        else if (c.form == tag("FREF"))
            reference(kids);
        else if (c.form == tag("CONS"))
        {
            for (const Chunk& k : kids)
                if (k.group)
                    for (const Chunk& cc : children(k.data, k.size, k.wide, &truncated))
                        connection(cc);
        }
        else if (c.form == tag("CONN"))
        {
            for (const Chunk& cc : kids)
                connection(cc);
        }
        else if (c.form == tag("SLCT"))
        {
            Node n;
            n.existing = true;
            for (const Chunk& k : kids)
            {
                if (k.tag == tag("SLCT"))
                {
                    n.name = std::string(reinterpret_cast<const char*>(k.data), k.size);
                    while (!n.name.empty() && n.name.back() == 0)
                        n.name.pop_back();
                    while (!n.name.empty() && n.name[0] == ':')
                        n.name.erase(n.name.begin());
                }
                else
                    attribute(k, n);
            }
            if (!n.name.empty())
                f.nodes.push_back(std::move(n));
        }
        else if (c.form == tag("FRDI"))
            loadState(kids);
        else
            node(c, kids);
    }

    void header(const std::vector<Chunk>& kids)
    {
        for (const Chunk& k : kids)
        {
            std::vector<std::string> s = nulStrings(k);
            std::string first = s.empty() ? std::string() : s[0];
            switch (k.tag)
            {
            case tag("VERS"): f.version = first; break;
            case tag("LUNI"): f.linearUnit = first; break;
            case tag("AUNI"): f.angleUnit = first; break;
            case tag("TUNI"): f.timeUnit = first; break;
            case tag("TDUR"): mTicks = std::strtod(first.c_str(), nullptr); break;
            case tag("FINF"):
                if (s.size() >= 2)
                    f.fileInfo[s[0]] = s[1];
                break;
            case tag("PLUG"): plugin(k); break;
            default: break;
            }
        }
        if (mTicks <= 0.0)
            mTicks = 141120000.0;
    }

    /** Plugin name, version, requires flags, then (node type name, 4-byte id) pairs. */
    void plugin(const Chunk& k)
    {
        Cursor c(k.data, k.size);
        std::string name = c.str();
        c.str();
        c.str();
        f.requires_.push_back(name);
        while (c.ok() && c.left() > 4)
        {
            std::string type = c.str();
            uint32_t id = c.u32();
            if (c.ok() && !type.empty())
                mPluginTypes[id] = type;
        }
    }

    /** Depth, path, namespace, two flag bytes (the second set when unloaded), reference node. Only the scene's own references count. */
    void loadState(const std::vector<Chunk>& kids)
    {
        for (const Chunk& k : kids)
        {
            if (k.tag != tag("FRDI"))
                continue;
            Cursor c(k.data, k.size);
            const uint32_t depth = c.u32();
            c.str();
            c.str();
            c.u8();
            const bool unloaded = c.u8() != 0;
            const std::string refNode = c.str();
            if (c.ok() && depth == 1 && unloaded)
                mUnloaded.insert(refNode);
        }
    }

    void reference(const std::vector<Chunk>& kids)
    {
        for (const Chunk& k : kids)
        {
            if (k.tag != tag("FREF"))
                continue;
            std::vector<std::string> s = nulStrings(k);
            if (s.empty() || s[0].empty())
                continue;
            Reference r{.path = s[0], .ns = s.size() > 1 ? s[1] : std::string()};
            if (s.size() > 2)
            {
                // Two flag bytes run into the reference node's name.
                size_t at = 0;
                while (at < s[2].size() && uint8_t(s[2][at]) < 32)
                    ++at;
                r.refNode = s[2].substr(at);
            }
            for (size_t i = s.size(); i-- > 2;)
                if (!s[i].empty() && std::all_of(s[i].begin(), s[i].end(), [](char ch) { return ch >= 32 && ch < 127; }))
                {
                    r.type = s[i];
                    break;
                }
            f.references.push_back(std::move(r));
        }
    }

    void connection(const Chunk& k)
    {
        if (k.tag != tag("CWFL"))
            return;
        Cursor c(k.data, k.size);
        uint8_t flags = c.u8();
        Connection con{.src = c.str(), .dst = c.str(), .nextAvailable = (flags & 1) != 0};
        for (std::string* s : {&con.src, &con.dst})
            while (!s->empty() && (*s)[0] == ':')
                s->erase(s->begin());
        if (c.ok())
            f.connections.push_back(std::move(con));
    }

    void node(const Chunk& c, const std::vector<Chunk>& kids)
    {
        if (kids.empty() || kids[0].tag != tag("CREA"))
            return;
        Node n;
        std::string_view known = typeForCode(c.form);
        if (auto it = mPluginTypes.find(c.form); it != mPluginTypes.end())
            n.type = it->second;
        else if (!known.empty())
            n.type = std::string(known);
        else
        {
            ++mUnknown[c.form];
            n.type = "unknown:" + tagName(c.form);
        }
        Cursor cur(kids[0].data, kids[0].size);
        uint8_t flags = cur.u8();
        n.shared = (flags & 0x01) != 0;
        n.name = cur.str();
        if (!(flags & 0x80) && cur.left() > 16)
            n.parent = cur.str();
        for (size_t i = 1; i < kids.size(); ++i)
            attribute(kids[i], n);
        mCurrentType = {};
        f.nodes.push_back(std::move(n));
    }

    void attribute(const Chunk& k, Node& n)
    {
        Cursor c(k.data, k.size);
        if (k.tag == tag("ATTR"))
        {
            AddedAttr a;
            std::string code = c.str();
            c.skip(3);
            c.u32();
            if (code == "aTYP")
                a.type = tagName(c.u32());
            else
            {
                c.u32();
                a.type = code.size() == 4 ? code.substr(1) : code;
            }
            a.longName = c.str();
            a.shortName = c.str();
            if (c.ok())
                n.added.push_back(std::move(a));
            return;
        }
        if (k.tag == tag("REFE"))
        {
            refEdits(c, n);
            return;
        }
        AttrValue v;
        v.attr = c.str();
        uint8_t flags = c.u8();
        if (flags & 0x01)
            v.lock = 1;
        if (flags & 0x08)
            v.size = int(c.u32());
        if (!c.ok())
            return;
        switch (k.tag)
        {
        case tag("FLGS"): break;
        case tag("DBLE"):
            if (sparsePath(splitAttr(v.attr)))
            {
                while (c.ok() && c.left() >= 4)
                {
                    uint32_t cnt = c.count(12);
                    v.nums.push_back(cnt);
                    for (uint32_t i = 0; i < cnt; ++i)
                    {
                        v.nums.push_back(double(c.u32()));
                        v.nums.push_back(c.f64());
                    }
                }
                break;
            }
            [[fallthrough]];
        case tag("DBL2"):
        case tag("DBL3"):
            while (c.left() >= 8)
                v.nums.push_back(c.f64());
            break;
        case tag("FLT2"):
        case tag("FLT3"):
            while (c.left() >= 4)
                v.nums.push_back(c.f32());
            break;
        case tag("STR "):
            v.type = "string";
            v.strs.push_back(c.str());
            break;
        case tag("STR#"):
        {
            v.type = "stringArray";
            uint32_t cnt = c.count(1);
            for (uint32_t i = 0; i < cnt && c.ok(); ++i)
                v.strs.push_back(c.str());
            break;
        }
        case tag("I32#"):
        {
            v.type = "Int32Array";
            uint32_t cnt = c.count(4);
            for (uint32_t i = 0; i < cnt; ++i)
                v.nums.push_back(double(int32_t(c.u32())));
            break;
        }
        case tag("DBL#"):
        {
            v.type = "doubleArray";
            uint32_t cnt = c.count(8);
            for (uint32_t i = 0; i < cnt; ++i)
                v.nums.push_back(c.f64());
            break;
        }
        case tag("PNT#"):
            v.type = "pointArray";
            // x, y, z, w per point, with no count ahead of them.
            while (c.left() >= 8)
                v.nums.push_back(c.f64());
            break;
        case tag("ATAL"):
        {
            v.type = "attributeAlias";
            uint32_t cnt = c.count(1);
            for (uint32_t i = 0; i < cnt && c.ok(); ++i)
                v.strs.push_back(c.str());
            break;
        }
        case tag("MATR"):
            if (c.left() == 128)
            {
                v.type = "matrix";
                for (int i = 0; i < 16; ++i)
                    v.nums.push_back(c.f64());
            }
            else
                v.type = "matrixXform";
            break;
        case tag("CMP#"):
        {
            v.type = "componentList";
            uint32_t items = c.count(8);
            for (uint32_t i = 0; i < items && c.ok(); ++i)
            {
                std::string kind = componentKind(c.u32());
                uint32_t ranges = c.count(8);
                for (uint32_t r = 0; r < ranges && c.ok(); ++r)
                {
                    int32_t lo = int32_t(c.u32()), hi = int32_t(c.u32());
                    // A negative start is "all of them" ([*]); the end is then the count.
                    if (lo < 0)
                        v.strs.push_back(kind + "[*]");
                    else
                        v.strs.push_back(lo == hi ? std::format("{}[{}]", kind, lo) : std::format("{}[{}:{}]", kind, lo, hi));
                }
            }
            break;
        }
        case tag("CMPD"):
            if (n.type.starts_with("animCurveT") && v.attr.starts_with("ktv"))
            {
                // Key times are ticks; keep frames of the file's time unit, as ASCII does.
                const double fps = fpsOf(f.timeUnit) > 0.0 ? fpsOf(f.timeUnit) : 24.0;
                while (c.left() >= 16)
                {
                    v.nums.push_back(double(int64_t(c.u64())) * fps / mTicks);
                    v.nums.push_back(c.f64());
                }
            }
            else if (c.left() % 8 == 0)
                while (c.left() >= 8)
                    v.nums.push_back(c.f64());
            break;
        case tag("MESH"):
        {
            std::string warn;
            v.type = "mesh";
            v.mesh = decodeMesh(c, &warn);
            if (!warn.empty())
                f.warnings.push_back(std::format("{}.{}: {}", n.name, v.attr, warn));
            break;
        }
        default:
            ++mSkippedValues[tagName(k.tag)];
            return;
        }
        n.attrs.push_back(std::move(v));
    }

    /** "ed", flag, owner reference node, then groups: count, (reference node, edit count, edits)... */
    void refEdits(Cursor& c, Node& n)
    {
        c.str();
        c.u8();
        c.str();
        while (c.ok() && c.left() >= 4)
        {
            uint32_t groups = c.count(5);
            for (uint32_t gi = 0; gi < groups && c.ok(); ++gi)
            {
                c.str();
                uint32_t count = c.count(4);
                for (uint32_t e = 0; e < count && c.ok(); ++e)
                {
                    RefEdit ed;
                    ed.kind = int(c.u32());
                    int strings = 0;
                    switch (ed.kind)
                    {
                    case 0: strings = 3; break;
                    case 1: strings = 4; break;
                    case 2: strings = 3; break;
                    case 3: strings = 2; break;
                    case 5: ed.args.push_back(std::to_string(c.u32())); strings = 4; break;
                    case 7: strings = 2; break;
                    case 8: strings = 2; break;
                    default:
                        f.warnings.push_back(std::format("{}: reference edit of unknown kind {}; the rest were ignored", n.name, ed.kind));
                        return;
                    }
                    for (int k = 0; k < strings; ++k)
                        ed.args.push_back(c.str());
                    if (c.ok())
                        n.refEdits.push_back(std::move(ed));
                }
            }
        }
    }

    double mTicks = 141120000.0;
    std::unordered_map<uint32_t, std::string> mPluginTypes;
    std::map<uint32_t, int> mUnknown;
    std::map<std::string, int> mSkippedValues;
    std::set<std::string> mUnloaded; // reference nodes of the references saved unloaded
    std::string mCurrentType;
};
} // namespace

File readBinary(std::span<const uint8_t> bytes)
{
    LILA_ZONE();

    Decoder d;
    d.f.binary = true;
    d.f.internalUnits = true;
    d.run(bytes);
    return std::move(d.f);
}
} // namespace lila::maya
