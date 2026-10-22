#include "fbx/FbxBinary.h"

#include "core/Profile.h"

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
} // namespace

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

void Record::write(std::string& out) const
{
    const size_t start = out.size();
    put(out, uint32_t(0)); // end offset, patched below
    put(out, uint32_t(mPropCount));
    put(out, uint32_t(mProps.size()));
    out.push_back(char(uint8_t(mName.size())));
    out += mName;
    out += mProps;
    for (const auto& c : mChildren)
        c->write(out);
    // Nested lists end in a null record; the SDK also writes one for records with nothing in them.
    if (!mChildren.empty() || mPropCount == 0)
        out.append(kNullRecordSize, '\0');
    patch32(out, start, uint32_t(out.size()));
}

std::string objectName(std::string_view name, std::string_view cls)
{
    std::string s(name);
    s.push_back('\0');
    s.push_back('\x01');
    s += cls;
    return s;
}

std::string encodeFile(const Record& root, uint32_t version)
{
    LILA_ZONE();

    std::string out;
    out.append("Kaydara FBX Binary  ", 20);
    out.push_back('\0');
    out.push_back('\x1a');
    out.push_back('\0');
    put(out, version);
    for (const auto& c : root.children())
        c->write(out);
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
} // namespace lila::fbx::bin
