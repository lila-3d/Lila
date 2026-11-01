#include "blend/BlendFile.h"

#include "core/Profile.h"

#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>

namespace lila::blend
{
namespace
{
template <typename T>
T load(const uint8_t* p)
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

double loadNumber(const std::string& type, const uint8_t* p)
{
    switch (type.size() > 0 ? type[0] : ' ')
    {
    case 'f': return type == "float" ? double(load<float>(p)) : 0.0;
    case 'd': return type == "double" ? load<double>(p) : 0.0;
    case 'c': return type == "char" ? double(int8_t(*p)) : 0.0;
    case 'b': return type == "bool" ? double(*p != 0) : 0.0;
    case 's': return type == "short" ? double(load<int16_t>(p)) : 0.0;
    case 'l': return type == "long" ? double(load<int32_t>(p)) : 0.0;
    case 'i':
        if (type == "int" || type == "int32_t")
            return double(load<int32_t>(p));
        if (type == "int8_t")
            return double(int8_t(*p));
        if (type == "int16_t")
            return double(load<int16_t>(p));
        if (type == "int64_t")
            return double(load<int64_t>(p));
        return 0.0;
    case 'u':
        if (type == "uchar" || type == "uint8_t")
            return double(*p);
        if (type == "ushort" || type == "uint16_t")
            return double(load<uint16_t>(p));
        if (type == "uint" || type == "uint32_t")
            return double(load<uint32_t>(p));
        if (type == "uint64_t")
            return double(load<uint64_t>(p));
        if (type == "ulong")
            return double(load<uint32_t>(p));
        return 0.0;
    default: return 0.0;
    }
}

bool isNumeric(const Field& f)
{
    return !f.pointer && f.elemSize > 0 && f.elemSize <= 8;
}

std::vector<uint8_t> unzstd(const std::vector<uint8_t>& in)
{
    std::vector<uint8_t> out;
    ZSTD_DStream* ds = ZSTD_createDStream();
    ZSTD_inBuffer src{in.data(), in.size(), 0};
    std::vector<uint8_t> chunk(ZSTD_DStreamOutSize());
    // Blender writes several frames plus a seek table, so stream rather than trust one frame's size.
    while (src.pos < src.size)
    {
        ZSTD_outBuffer dst{chunk.data(), chunk.size(), 0};
        const size_t r = ZSTD_decompressStream(ds, &dst, &src);
        if (ZSTD_isError(r))
        {
            ZSTD_freeDStream(ds);
            throw std::runtime_error("the file is damaged (zstd)");
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + std::ptrdiff_t(dst.pos));
    }
    ZSTD_freeDStream(ds);
    return out;
}

std::vector<uint8_t> gunzip(const std::vector<uint8_t>& in)
{
    std::vector<uint8_t> out;
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
        throw std::runtime_error("can't start gzip");
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = uInt(in.size());
    std::vector<uint8_t> chunk(1 << 20);
    int r = Z_OK;
    while (r != Z_STREAM_END)
    {
        zs.next_out = chunk.data();
        zs.avail_out = uInt(chunk.size());
        r = inflate(&zs, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END)
        {
            inflateEnd(&zs);
            throw std::runtime_error("the file is damaged (gzip)");
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + std::ptrdiff_t(chunk.size() - zs.avail_out));
    }
    inflateEnd(&zs);
    return out;
}
} // namespace

const Field* Struct::field(std::string_view name) const
{
    auto it = byName.find(std::string(name));
    return it == byName.end() ? nullptr : &fields[it->second];
}

// View ----------------------------------------------------------------------------------------------------------------

const std::string& View::typeName() const
{
    static const std::string empty;
    return mType ? mType->name : empty;
}

double View::num(std::string_view name, size_t i, double fallback) const
{
    const Field* f = field(name);
    if (!mData || !f || !isNumeric(*f) || i >= f->count)
        return fallback;
    return loadNumber(f->type, mData + f->offset + i * f->elemSize);
}

int64_t View::integer(std::string_view name, size_t i, int64_t fallback) const
{
    const Field* f = field(name);
    if (!mData || !f || !isNumeric(*f) || i >= f->count)
        return fallback;
    const uint8_t* p = mData + f->offset + i * f->elemSize;
    if (f->type == "int64_t" || f->type == "uint64_t")
        return load<int64_t>(p);
    return int64_t(loadNumber(f->type, p));
}

glm::dvec3 View::vec3(std::string_view name, glm::dvec3 fallback) const
{
    const Field* f = field(name);
    if (!f || f->count < 3)
        return fallback;
    return {num(name, 0), num(name, 1), num(name, 2)};
}

glm::dvec4 View::vec4(std::string_view name, glm::dvec4 fallback) const
{
    const Field* f = field(name);
    if (!f || f->count < 4)
        return fallback;
    return {num(name, 0), num(name, 1), num(name, 2), num(name, 3)};
}

glm::dmat4 View::mat4(std::string_view name) const
{
    glm::dmat4 m(1.0);
    const Field* f = field(name);
    if (!f || f->count < 16)
        return m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m[c][r] = num(name, size_t(c * 4 + r));
    return m;
}

glm::dmat3 View::mat3(std::string_view name) const
{
    glm::dmat3 m(1.0);
    const Field* f = field(name);
    if (!f || f->count < 9)
        return m;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            m[c][r] = num(name, size_t(c * 3 + r));
    return m;
}

std::string View::str(std::string_view name) const
{
    const Field* f = field(name);
    if (!mData || !f || f->pointer)
        return {};
    const char* s = reinterpret_cast<const char*>(mData + f->offset);
    return std::string(s, strnlen(s, size_t(f->elemSize) * f->count));
}

uint64_t View::ptr(std::string_view name, size_t i) const
{
    const Field* f = field(name);
    if (!mData || !f || !f->pointer || i >= f->count)
        return 0;
    return load<uint64_t>(mData + f->offset + i * 8);
}

View View::sub(std::string_view name) const
{
    const Field* f = field(name);
    if (!mData || !f || f->pointer)
        return {};
    return {mFile, mFile->type(f->type), mData + f->offset, mScope};
}

const Block* View::block(std::string_view name, size_t i) const
{
    const Field* f = field(name);
    const uint64_t a = ptr(name, i);
    if (!a || !f)
        return nullptr;
    const Struct* t = mFile->type(f->type);
    return mFile->block(a, mScope, t && t->isId);
}

View View::deref(std::string_view name, size_t i) const
{
    const Field* f = field(name);
    const uint64_t a = ptr(name, i);
    if (!a || !f)
        return {};
    const Struct* t = mFile->type(f->type);
    const bool generic = !t || f->type == "ID" || f->type == "void";
    const Block* b = mFile->block(a, mScope, t && t->isId);
    if (!b)
        return {};
    const Struct* bt = mFile->typeOf(*b);
    // A void* or ID* takes its block's type; a typed pointer keeps its own (the block may hold raw bytes).
    const Struct* use = generic ? bt : t;
    return {mFile, use, b->data, b->isId() ? int32_t(b - mFile->blocks().data()) : b->owner};
}

std::string View::cstr(std::string_view name) const
{
    const Block* b = block(name);
    if (!b)
        return {};
    const char* s = reinterpret_cast<const char*>(b->data);
    return std::string(s, strnlen(s, b->size));
}

View View::element(size_t i) const
{
    if (!mData || !mType)
        return {};
    return {mFile, mType, mData + i * mType->size, mScope};
}

std::vector<View> View::list(std::string_view name, std::string_view itemType) const
{
    std::vector<View> out;
    const Field* f = field(name);
    if (!mData || !f || f->pointer)
        return out;
    uint64_t a = load<uint64_t>(mData + f->offset);
    const Struct* t = itemType.empty() ? nullptr : mFile->type(itemType);
    for (int guard = 0; a && guard < 10000000; ++guard)
    {
        const Block* b = mFile->block(a, mScope);
        if (!b)
            break;
        View v{mFile, t ? t : mFile->typeOf(*b), b->data, b->isId() ? int32_t(b - mFile->blocks().data()) : b->owner};
        out.push_back(v);
        a = load<uint64_t>(b->data); // every ListBase item starts with next
    }
    return out;
}

std::vector<View> View::array(std::string_view name, size_t n, std::string_view type) const
{
    std::vector<View> out;
    const Field* f = field(name);
    const Block* b = block(name);
    if (!b || !f)
        return out;
    const Struct* t = mFile->type(type.empty() ? std::string_view(f->type) : type);
    if (!t || t->size == 0)
        return out;
    n = std::min(n, b->size / t->size);
    for (size_t k = 0; k < n; ++k)
        out.push_back({mFile, t, b->data + k * t->size, b->owner});
    return out;
}

std::vector<View> View::pointerArray(std::string_view name, size_t n) const
{
    std::vector<View> out;
    const Block* b = block(name);
    const Field* f = field(name);
    if (!b || !f)
        return out;
    n = std::min(n, b->size / 8);
    const Struct* t = mFile->type(f->type);
    for (size_t k = 0; k < n; ++k)
    {
        const uint64_t a = load<uint64_t>(b->data + k * 8);
        const Block* target = a ? mFile->block(a, b->owner, t && t->isId) : nullptr;
        if (!target)
        {
            out.emplace_back();
            continue;
        }
        const Struct* tt = (!t || f->type == "ID" || f->type == "void") ? mFile->typeOf(*target) : t;
        out.push_back({mFile, tt, target->data, target->isId() ? int32_t(target - mFile->blocks().data()) : target->owner});
    }
    return out;
}

std::string View::idName() const
{
    const std::string n = sub("id").str("name");
    return n.size() > 2 ? n.substr(2) : n;
}

std::string View::idCode() const
{
    const std::string n = sub("id").str("name");
    return n.substr(0, 2);
}

View View::library() const
{
    return sub("id").deref("lib");
}

// File ----------------------------------------------------------------------------------------------------------------

File::File(const fs::path& path) : mPath(path)
{
    LILA_ZONE();

    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw std::runtime_error("can't read the file");
        in.seekg(0, std::ios::end);
        mBytes.resize(size_t(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(mBytes.data()), std::streamsize(mBytes.size()));
    }
    if (mBytes.size() >= 4 && load<uint32_t>(mBytes.data()) == ZSTD_MAGICNUMBER)
        mBytes = unzstd(mBytes);
    else if (mBytes.size() >= 2 && mBytes[0] == 0x1f && mBytes[1] == 0x8b)
        mBytes = gunzip(mBytes);
    readHeaderAndBlocks();
}

std::string File::versionString() const
{
    return std::format("{}.{}", mVersion / 100, mVersion % 100);
}

void File::readHeaderAndBlocks()
{
    LILA_ZONE();

    const uint8_t* d = mBytes.data();
    const size_t n = mBytes.size();
    if (n < 12 || std::memcmp(d, "BLENDER", 7) != 0)
        throw std::runtime_error("not a Blender file");
    size_t pos = 0;
    bool large = false;
    if (n >= 17 && std::memcmp(d + 7, "17-01v", 6) == 0)
    {
        // Blender 5: "BLENDER" + header size "17" + "-" + BHead format "01" + "v" + 4-digit version.
        large = true;
        mVersion = std::atoi(std::string(reinterpret_cast<const char*>(d + 13), 4).c_str());
        pos = 17;
    }
    else
    {
        if (d[7] == '_')
            throw std::runtime_error("saved by a 32-bit Blender; open and save it in a current Blender first");
        if (d[8] == 'V')
            throw std::runtime_error("saved on a big-endian machine; open and save it in a current Blender first");
        if (d[7] != '-' || d[8] != 'v')
            throw std::runtime_error("unknown Blender file header");
        mVersion = std::atoi(std::string(reinterpret_cast<const char*>(d + 9), 3).c_str());
        pos = 12;
    }

    int32_t owner = -1;
    const size_t headSize = large ? 32 : 24;
    while (pos + headSize <= n)
    {
        const uint8_t* h = d + pos;
        Block b;
        std::memcpy(b.code, h, 4);
        if (large)
        {
            b.sdna = load<uint32_t>(h + 4);
            b.address = load<uint64_t>(h + 8);
            b.size = size_t(load<uint64_t>(h + 16));
            b.count = uint32_t(load<uint64_t>(h + 24));
        }
        else
        {
            b.size = size_t(load<int32_t>(h + 4));
            b.address = load<uint64_t>(h + 8);
            b.sdna = load<uint32_t>(h + 16);
            b.count = load<uint32_t>(h + 20);
        }
        if (b.codeView() == "ENDB")
            break;
        b.data = h + headSize;
        if (pos + headSize + b.size > n)
            throw std::runtime_error("the file is truncated");
        const uint32_t index = uint32_t(mBlocks.size());
        if (b.isId())
            owner = int32_t(index);
        b.owner = b.isId() ? int32_t(index) : owner;
        mBlocks.push_back(b);
        pos += headSize + b.size;
    }

    const Block* dna = nullptr;
    for (const Block& b : mBlocks)
        if (b.codeView() == "DNA1")
            dna = &b;
    if (!dna)
        throw std::runtime_error("the file has no struct descriptions (DNA1)");
    readSdna(*dna);

    for (uint32_t i = 0; i < mBlocks.size(); ++i)
    {
        const Block& b = mBlocks[i];
        if (!b.address)
            continue;
        if (b.isId())
            mIdByAddress.emplace(b.address, i);
        else
            mScoped.emplace(std::pair{b.owner, b.address}, i);
        mFirstByAddress.emplace(b.address, i);
    }
}

void File::readSdna(const Block& dna)
{
    LILA_ZONE();

    const uint8_t* d = dna.data;
    const size_t end = dna.size;
    size_t pos = 8; // "SDNANAME"
    auto need = [&](size_t bytes) {
        if (pos + bytes > end)
            throw std::runtime_error("the file's struct descriptions are damaged");
    };
    auto align = [&] { pos = (pos + 3) & ~size_t(3); };
    auto strings = [&] {
        need(4);
        std::vector<std::string> out(load<uint32_t>(d + pos));
        pos += 4;
        for (std::string& s : out)
        {
            const size_t len = strnlen(reinterpret_cast<const char*>(d + pos), end - pos);
            s.assign(reinterpret_cast<const char*>(d + pos), len);
            pos += len + 1;
        }
        align();
        return out;
    };
    const std::vector<std::string> names = strings();
    pos += 4; // "TYPE"
    const std::vector<std::string> types = strings();
    pos += 4; // "TLEN"
    need(types.size() * 2);
    std::vector<uint16_t> lengths(types.size());
    for (size_t i = 0; i < types.size(); ++i, pos += 2)
        lengths[i] = load<uint16_t>(d + pos);
    align();
    pos += 4; // "STRC"
    need(4);
    const uint32_t count = load<uint32_t>(d + pos);
    pos += 4;
    mStructs.reserve(count);
    for (uint32_t s = 0; s < count; ++s)
    {
        need(4);
        Struct st;
        const uint16_t typeIndex = load<uint16_t>(d + pos);
        const uint16_t members = load<uint16_t>(d + pos + 2);
        pos += 4;
        need(size_t(members) * 4);
        st.name = types.at(typeIndex);
        st.size = lengths.at(typeIndex);
        uint32_t offset = 0;
        for (uint16_t m = 0; m < members; ++m, pos += 4)
        {
            const uint16_t memberType = load<uint16_t>(d + pos);
            const std::string& raw = names.at(load<uint16_t>(d + pos + 2));
            Field f;
            f.type = types.at(memberType);
            f.pointer = !raw.empty() && (raw[0] == '*' || raw[0] == '(');
            f.offset = offset;
            f.elemSize = f.pointer ? 8 : lengths.at(memberType);
            if (raw.starts_with("(*"))
                f.name = raw.substr(2, raw.find(')') - 2);
            else
                for (size_t i = 0; i < raw.size(); ++i)
                    if (raw[i] == '[')
                    {
                        f.count *= uint32_t(std::stoul(raw.substr(i + 1)));
                        i = raw.find(']', i);
                    }
                    else if (raw[i] != '*')
                        f.name += raw[i];
            offset += f.elemSize * f.count;
            st.byName.emplace(f.name, uint32_t(st.fields.size()));
            st.fields.push_back(std::move(f));
        }
        st.isId = !st.fields.empty() && st.fields[0].name == "id" && st.fields[0].type == "ID" && !st.fields[0].pointer;
        mStructByName.emplace(st.name, s);
        mStructs.push_back(std::move(st));
    }
}

const Struct* File::type(std::string_view name) const
{
    auto it = mStructByName.find(std::string(name));
    return it == mStructByName.end() ? nullptr : &mStructs[it->second];
}

std::vector<View> File::ids(std::string_view code) const
{
    std::vector<View> out;
    for (const Block& b : mBlocks)
        if (b.isId() && b.codeView() == code)
            out.push_back(viewOf(b));
    return out;
}

const Block* File::block(uint64_t address, int32_t scope, bool preferId) const
{
    if (!address)
        return nullptr;
    if (preferId)
        if (auto it = mIdByAddress.find(address); it != mIdByAddress.end())
            return &mBlocks[it->second];
    if (auto it = mScoped.find({scope, address}); it != mScoped.end())
        return &mBlocks[it->second];
    if (auto it = mIdByAddress.find(address); it != mIdByAddress.end())
        return &mBlocks[it->second];
    if (auto it = mFirstByAddress.find(address); it != mFirstByAddress.end())
        return &mBlocks[it->second];
    return nullptr;
}

View File::at(uint64_t address, int32_t scope, const Struct* t) const
{
    const Block* b = block(address, scope, t && t->isId);
    if (!b)
        return {};
    return {this, t ? t : typeOf(*b), b->data, b->isId() ? int32_t(b - mBlocks.data()) : b->owner};
}

View File::viewOf(const Block& b) const
{
    return {this, typeOf(b), b.data, b.isId() ? int32_t(&b - mBlocks.data()) : b.owner};
}

fs::path File::resolvePath(std::string_view raw, const View& library) const
{
    std::string p(raw);
    std::replace(p.begin(), p.end(), '\\', '/');
    if (!p.starts_with("//"))
        return fs::path(std::u8string(p.begin(), p.end()));
    fs::path base = mPath.parent_path();
    if (library)
    {
        // A library's own path is relative to this file; its data's paths are relative to the library.
        const fs::path lib = resolvePath(library.str("filepath"));
        base = lib.parent_path();
    }
    const std::string rest = p.substr(2);
    return (base / fs::path(std::u8string(rest.begin(), rest.end()))).lexically_normal();
}

bool isBlendFile(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".blend")
        return true;
    return ext.size() > 6 && ext.starts_with(".blend") && std::all_of(ext.begin() + 6, ext.end(), [](char c) { return c >= '0' && c <= '9'; });
}
} // namespace lila::blend
