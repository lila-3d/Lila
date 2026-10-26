#pragma once

#include "core/Export.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * FBX binary node records, version 7.4 (32-bit offsets). A record is a name, a list of typed properties and nested
 * records. encodeFile deflates the larger arrays, as the FBX SDK does.
 */
namespace lila::fbx::bin
{
class LILA_SCENE_API Record
{
public:
    explicit Record(std::string name) : mName(std::move(name)) {}
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;
    Record(Record&&) = default;
    Record& operator=(Record&&) = default;

    const std::string& name() const { return mName; }

    Record& add(std::string name);
    /** Properties70 "P" entry with its four header strings; push the value(s) onto the returned record. */
    Record& addP(std::string_view name, std::string_view type, std::string_view label, std::string_view flags);

    Record& i16(int16_t v);
    Record& boolean(bool v);
    Record& i32(int32_t v);
    Record& i64(int64_t v);
    Record& f32(float v);
    Record& f64(double v);
    Record& str(std::string_view v);
    Record& raw(std::span<const uint8_t> v);
    Record& i32s(std::span<const int32_t> v);
    Record& i64s(std::span<const int64_t> v);
    Record& f32s(std::span<const float> v);
    Record& f64s(std::span<const double> v);
    Record& bools(std::span<const uint8_t> v);

    size_t propertyCount() const { return mPropCount; }
    const std::vector<std::unique_ptr<Record>>& children() const { return mChildren; }

private:
    friend class Encoder;

    /** An array property in mProps: where its encoding field sits and its raw byte count. */
    struct Array
    {
        size_t at = 0;
        size_t bytes = 0;
    };

    template <class T> Record& scalar(char type, T v);
    template <class T> Record& array(char type, std::span<const T> v);

    std::string mName;
    std::string mProps; // encoded property bytes, arrays raw
    size_t mPropCount = 0;
    std::vector<Array> mArrays;
    std::vector<std::unique_ptr<Record>> mChildren;
};

/** "Name\x00\x01Class", the binary form of an object name. */
LILA_SCENE_API std::string objectName(std::string_view name, std::string_view cls);

/** Arrays smaller than this stay raw, as the FBX SDK leaves them. */
constexpr size_t kDeflateMinBytes = 128;
/** Arrays are deflated in blocks of this size, in parallel. */
constexpr size_t kDeflateBlockBytes = size_t(1) << 20;

/**
 * Header, root's children as the top-level records, the closing null record and the footer. With compress, arrays of
 * kDeflateMinBytes or more are written as zlib streams (encoding 1) when that makes them smaller.
 */
LILA_SCENE_API std::string encodeFile(const Record& root, uint32_t version = 7400, bool compress = true);

/**
 * One zlib stream (header, deflate data, adler32 trailer) of data, deflated pigz-style: blockBytes blocks compressed
 * in parallel, each primed with the 32 KB before it, joined by sync flushes. Any inflater reads it.
 */
LILA_SCENE_API std::string zlibDeflate(std::span<const uint8_t> data, size_t blockBytes = kDeflateBlockBytes);
} // namespace lila::fbx::bin
