#include "fbx/FbxBinary.h"

#include "core/Profile.h"

#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace lila::fbx::bin
{
namespace
{
constexpr size_t kNullRecordSize = 13;

// The FBX SDK checks FileId and CreationTime against this footer id, so the three go together.
constexpr uint8_t kFooterId[16] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
constexpr uint8_t kFooterMagic[16] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};

template <class T> void put(std::string& out, T v)
{
    char b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    out.append(b, sizeof(T));
}

void patch32(std::string& out, size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); }

// Levels 2 and 3 save about 0.1% on dense meshes and cost 10 to 15% more export time.
constexpr int kLevel = 1;
constexpr size_t kWindow = 32768;

/** A raw deflate stream (no zlib header or trailer), reset for each block. */
struct Deflater
{
    z_stream s{};
    Deflater() { deflateInit2(&s, kLevel, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY); }
    ~Deflater() { deflateEnd(&s); }
    Deflater(const Deflater&) = delete;
    Deflater& operator=(const Deflater&) = delete;
};

struct Block
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t dict = 0; // bytes before data to prime the window with
    bool last = false;
    std::string out;
    uLong adler = 1;
};

void deflateBlock(Deflater& d, Block& b)
{
    LILA_ZONE();

    z_stream& s = d.s;
    deflateReset(&s);
    if (b.dict)
        deflateSetDictionary(&s, b.data - b.dict, uInt(b.dict));
    b.out.resize(deflateBound(&s, uLong(b.size)) + 16);
    s.next_in = const_cast<Bytef*>(b.data);
    s.avail_in = uInt(b.size);
    const int flush = b.last ? Z_FINISH : Z_SYNC_FLUSH;
    for (;;)
    {
        s.next_out = reinterpret_cast<Bytef*>(b.out.data()) + s.total_out;
        s.avail_out = uInt(b.out.size() - s.total_out);
        const int r = deflate(&s, flush);
        if (b.last ? r == Z_STREAM_END : s.avail_in == 0 && s.avail_out != 0)
            break;
        b.out.resize(b.out.size() * 2);
    }
    b.out.resize(s.total_out);
    b.adler = adler32_z(1, b.data, b.size);
}

/** A zlib stream kept as its pieces (header, blocks, trailer), so it is copied once, into the file. */
struct Deflated
{
    std::vector<std::string> parts;
    size_t size = 0;

    void appendTo(std::string& out) const
    {
        for (const std::string& p : parts)
            out += p;
    }
};

/** zlib streams of every input, all their blocks deflated in one parallel loop. */
std::vector<Deflated> deflateAll(const std::vector<std::span<const uint8_t>>& inputs, size_t blockBytes)
{
    LILA_ZONE();

    std::vector<Block> blocks;
    std::vector<size_t> firstBlock(inputs.size() + 1, 0);
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        std::span<const uint8_t> in = inputs[i];
        firstBlock[i] = blocks.size();
        for (size_t at = 0; at < in.size() || at == 0; at += blockBytes)
        {
            Block& b = blocks.emplace_back();
            b.data = in.data() + at;
            b.size = std::min(blockBytes, in.size() - at);
            b.dict = std::min(at, kWindow);
            b.last = at + blockBytes >= in.size();
        }
    }
    firstBlock[inputs.size()] = blocks.size();

    tbb::enumerable_thread_specific<Deflater> deflaters;
    tbb::parallel_for(size_t(0), blocks.size(), [&](size_t i) { deflateBlock(deflaters.local(), blocks[i]); });

    std::vector<Deflated> streams(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        Deflated& z = streams[i];
        z.parts.reserve(firstBlock[i + 1] - firstBlock[i] + 2);
        // FLEVEL 0 (fastest), with FCHECK making CMF and FLG together a multiple of 31.
        z.parts.emplace_back("\x78\x01", 2);
        uLong adler = 1;
        for (size_t k = firstBlock[i]; k < firstBlock[i + 1]; ++k)
        {
            adler = adler32_combine(adler, blocks[k].adler, static_cast<z_off_t>(blocks[k].size));
            z.parts.push_back(std::move(blocks[k].out));
        }
        std::string& trailer = z.parts.emplace_back();
        for (int shift = 24; shift >= 0; shift -= 8)
            trailer.push_back(char(uint8_t(adler >> shift)));
        for (const std::string& p : z.parts)
            z.size += p.size();
    }
    return streams;
}
} // namespace

/** Writes records, each array that deflates smaller as its zlib stream. */
class Encoder
{
public:
    Encoder(const Record& root, bool compress)
    {
        if (!compress)
            return;
        std::vector<std::span<const uint8_t>> inputs;
        std::vector<size_t> slots;
        gather(root, inputs, slots);
        mPacked.resize(mCount);
        std::vector<Deflated> streams = deflateAll(inputs, kDeflateBlockBytes);
        for (size_t i = 0; i < streams.size(); ++i)
            if (streams[i].size < inputs[i].size())
            {
                mSaved += inputs[i].size() - streams[i].size;
                mPacked[slots[i]] = std::move(streams[i]);
            }
    }

    /** At least the bytes write will produce. */
    size_t sizeBound(const Record& r) const
    {
        size_t n = 13 + r.mName.size() + r.mProps.size() + kNullRecordSize;
        for (const auto& c : r.mChildren)
            n += sizeBound(*c);
        return n;
    }

    size_t saved() const { return mSaved; }

    void write(const Record& r, std::string& out)
    {
        const size_t first = mNext;
        mNext += r.mArrays.size();
        size_t propBytes = r.mProps.size();
        for (size_t k = 0; k < r.mArrays.size(); ++k)
            if (const Deflated* z = packed(first + k))
                propBytes -= r.mArrays[k].bytes - z->size;

        const size_t start = out.size();
        put(out, uint32_t(0)); // end offset, patched below
        put(out, uint32_t(r.mPropCount));
        put(out, uint32_t(propBytes));
        out.push_back(char(uint8_t(r.mName.size())));
        out += r.mName;
        size_t pos = 0;
        for (size_t k = 0; k < r.mArrays.size(); ++k)
        {
            const Deflated* z = packed(first + k);
            if (!z)
                continue;
            const Record::Array& a = r.mArrays[k];
            out.append(r.mProps, pos, a.at - pos);
            put(out, uint32_t(1));
            put(out, uint32_t(z->size));
            z->appendTo(out);
            pos = a.at + 8 + a.bytes;
        }
        out.append(r.mProps, pos);
        for (const auto& c : r.mChildren)
            write(*c, out);
        // Nested lists end in a null record; the SDK also writes one for records with nothing in them.
        if (!r.mChildren.empty() || r.mPropCount == 0)
            out.append(kNullRecordSize, '\0');
        patch32(out, start, uint32_t(out.size()));
    }

private:
    /** Every array in write order; the ones worth deflating go to inputs, with their index in slots. */
    void gather(const Record& r, std::vector<std::span<const uint8_t>>& inputs, std::vector<size_t>& slots)
    {
        for (const Record::Array& a : r.mArrays)
        {
            if (a.bytes >= kDeflateMinBytes)
            {
                inputs.emplace_back(reinterpret_cast<const uint8_t*>(r.mProps.data()) + a.at + 8, a.bytes);
                slots.push_back(mCount);
            }
            ++mCount;
        }
        for (const auto& c : r.mChildren)
            gather(*c, inputs, slots);
    }

    const Deflated* packed(size_t i) const { return i < mPacked.size() && mPacked[i].size ? &mPacked[i] : nullptr; }

    std::vector<Deflated> mPacked; // per array in write order; empty stays raw
    size_t mCount = 0;
    size_t mSaved = 0;
    size_t mNext = 0;
};

Record& Record::add(std::string name)
{
    mChildren.push_back(std::make_unique<Record>(std::move(name)));
    return *mChildren.back();
}

Record& Record::addP(std::string_view name, std::string_view type, std::string_view label, std::string_view flags)
{
    Record& p = add("P");
    p.str(name).str(type).str(label).str(flags);
    return p;
}

template <class T> Record& Record::scalar(char type, T v)
{
    mProps.push_back(type);
    put(mProps, v);
    ++mPropCount;
    return *this;
}

template <class T> Record& Record::array(char type, std::span<const T> v)
{
    mProps.push_back(type);
    put(mProps, uint32_t(v.size()));
    mArrays.push_back({mProps.size(), v.size_bytes()});
    put(mProps, uint32_t(0)); // encoding: raw
    put(mProps, uint32_t(v.size_bytes()));
    mProps.append(reinterpret_cast<const char*>(v.data()), v.size_bytes());
    ++mPropCount;
    return *this;
}

Record& Record::i16(int16_t v) { return scalar('Y', v); }
Record& Record::boolean(bool v) { return scalar('C', uint8_t(v ? 1 : 0)); }
Record& Record::i32(int32_t v) { return scalar('I', v); }
Record& Record::i64(int64_t v) { return scalar('L', v); }
Record& Record::f32(float v) { return scalar('F', v); }
Record& Record::f64(double v) { return scalar('D', v); }

Record& Record::str(std::string_view v)
{
    mProps.push_back('S');
    put(mProps, uint32_t(v.size()));
    mProps.append(v);
    ++mPropCount;
    return *this;
}

Record& Record::raw(std::span<const uint8_t> v)
{
    mProps.push_back('R');
    put(mProps, uint32_t(v.size()));
    mProps.append(reinterpret_cast<const char*>(v.data()), v.size());
    ++mPropCount;
    return *this;
}

Record& Record::i32s(std::span<const int32_t> v) { return array('i', v); }
Record& Record::i64s(std::span<const int64_t> v) { return array('l', v); }
Record& Record::f32s(std::span<const float> v) { return array('f', v); }
Record& Record::f64s(std::span<const double> v) { return array('d', v); }
Record& Record::bools(std::span<const uint8_t> v) { return array('b', v); }

std::string objectName(std::string_view name, std::string_view cls)
{
    std::string s(name);
    s.push_back('\0');
    s.push_back('\x01');
    s += cls;
    return s;
}

std::string encodeFile(const Record& root, uint32_t version, bool compress)
{
    LILA_ZONE();

    Encoder encoder(root, compress);

    std::string out;
    size_t bound = 27 + 2 * kNullRecordSize + 200 - encoder.saved();
    for (const auto& c : root.children())
        bound += encoder.sizeBound(*c);
    out.reserve(bound);
    out.append("Kaydara FBX Binary  ", 20);
    out.push_back('\0');
    out.push_back('\x1a');
    out.push_back('\0');
    put(out, version);
    for (const auto& c : root.children())
        encoder.write(*c, out);
    out.append(kNullRecordSize, '\0');

    out.append(reinterpret_cast<const char*>(kFooterId), sizeof(kFooterId));
    out.append(4, '\0');
    size_t pad = ((out.size() + 15) & ~size_t(15)) - out.size();
    out.append(pad ? pad : 16, '\0');
    put(out, version);
    out.append(120, '\0');
    out.append(reinterpret_cast<const char*>(kFooterMagic), sizeof(kFooterMagic));
    return out;
}

std::string zlibDeflate(std::span<const uint8_t> data, size_t blockBytes)
{
    Deflated z = std::move(deflateAll({data}, std::clamp<size_t>(blockBytes, 1, size_t(1) << 30)).front());
    std::string out;
    out.reserve(z.size);
    z.appendTo(out);
    return out;
}
} // namespace lila::fbx::bin
