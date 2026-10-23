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
 * records. Arrays are written uncompressed, which every reader accepts.
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

    void write(std::string& out) const;

private:
    template <class T> Record& scalar(char type, T v);
    template <class T> Record& array(char type, std::span<const T> v);

    std::string mName;
    std::string mProps; // encoded property bytes
    size_t mPropCount = 0;
    std::vector<std::unique_ptr<Record>> mChildren;
};

/** "Name\x00\x01Class", the binary form of an object name. */
LILA_SCENE_API std::string objectName(std::string_view name, std::string_view cls);

/** Header, root's children as the top-level records, the closing null record and the footer. */
LILA_SCENE_API std::string encodeFile(const Record& root, uint32_t version = 7400);
} // namespace lila::fbx::bin
