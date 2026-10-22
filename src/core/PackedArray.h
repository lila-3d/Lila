#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace lila::packed
{
/**
 * A numeric array carried inside a json document as a binary value, so big arrays (mesh points, indices, UVs) skip
 * one json node per number on their way into a binary scene file. The subtype holds the element type and the row
 * width (3 for a list of vec3): subtype = dtype | width << 8. Anything that wants plain json calls unpackAll() first.
 */
enum class DType : uint8_t
{
    F64 = 1,
    F32 = 2,
    I32 = 3,
    U32 = 4,
    Bool = 5, // one byte each, 0 or 1
};

inline size_t dtypeSize(DType t)
{
    return t == DType::F64 ? 8 : t == DType::Bool ? 1 : 4;
}

inline const char* dtypeName(DType t)
{
    switch (t)
    {
    case DType::F64: return "f64";
    case DType::F32: return "f32";
    case DType::I32: return "i32";
    case DType::U32: return "u32";
    case DType::Bool: return "bool";
    }
    return "f64";
}

inline bool dtypeFromName(std::string_view name, DType& out)
{
    for (DType t : {DType::F64, DType::F32, DType::I32, DType::U32, DType::Bool})
        if (name == dtypeName(t))
            return out = t, true;
    return false;
}

template <class T> constexpr DType dtypeOf()
{
    if constexpr (std::is_same_v<T, double>)
        return DType::F64;
    else if constexpr (std::is_same_v<T, float>)
        return DType::F32;
    else if constexpr (std::is_same_v<T, int32_t>)
        return DType::I32;
    else if constexpr (std::is_same_v<T, bool>)
        return DType::Bool;
    else
    {
        static_assert(std::is_same_v<T, uint32_t>);
        return DType::U32;
    }
}

/** width > 1 makes rows of that many values, like the nested arrays json would hold. Empty data is an empty array. */
template <class T> nlohmann::json make(std::span<const T> values, int width = 1)
{
    if (values.empty())
        return nlohmann::json::array();
    nlohmann::json::binary_t::container_type bytes(values.size_bytes());
    std::memcpy(bytes.data(), values.data(), values.size_bytes());
    return nlohmann::json::binary(std::move(bytes), uint64_t(dtypeOf<T>()) | uint64_t(width) << 8);
}

/** A packed array of n uninitialised values for the caller to fill through `out`; n == 0 gives an empty array. */
template <class T> nlohmann::json alloc(size_t n, T*& out, int width = 1)
{
    out = nullptr;
    if (n == 0)
        return nlohmann::json::array();
    nlohmann::json j = nlohmann::json::binary(nlohmann::json::binary_t::container_type(n * sizeof(T)),
                                              uint64_t(dtypeOf<T>()) | uint64_t(width) << 8);
    out = reinterpret_cast<T*>(j.get_binary().data());
    return j;
}

inline bool is(const nlohmann::json& j)
{
    return j.is_binary() && j.get_binary().has_subtype() && (j.get_binary().subtype() & 0xFF) >= 1 &&
           (j.get_binary().subtype() & 0xFF) <= 5;
}

inline DType dtype(const nlohmann::json& j)
{
    return DType(j.get_binary().subtype() & 0xFF);
}

inline int width(const nlohmann::json& j)
{
    return std::max(1, int(j.get_binary().subtype() >> 8));
}

inline size_t count(const nlohmann::json& j)
{
    return j.get_binary().size() / dtypeSize(dtype(j));
}

/** Element i of raw values of the given type, as json (integers stay integers). */
inline nlohmann::json element(DType t, const uint8_t* data, size_t i)
{
    switch (t)
    {
    case DType::F64: { double v; std::memcpy(&v, data + i * 8, 8); return v; }
    case DType::F32: { float v; std::memcpy(&v, data + i * 4, 4); return double(v); }
    case DType::I32: { int32_t v; std::memcpy(&v, data + i * 4, 4); return v; }
    case DType::U32: { uint32_t v; std::memcpy(&v, data + i * 4, 4); return v; }
    case DType::Bool: return data[i] != 0;
    }
    return nullptr;
}

/** Raw values as the plain json array they stand for: flat, or rows of `width`. */
inline nlohmann::json toArray(DType t, const uint8_t* data, size_t n, int width)
{
    nlohmann::json::array_t out;
    if (width <= 1)
    {
        out.reserve(n);
        for (size_t i = 0; i < n; ++i)
            out.emplace_back(element(t, data, i));
        return out;
    }
    out.reserve(n / size_t(width) + 1);
    for (size_t r = 0; r * size_t(width) < n; ++r)
    {
        nlohmann::json::array_t row;
        row.reserve(size_t(width));
        for (size_t c = 0; c < size_t(width); ++c)
            row.emplace_back(r * width + c < n ? element(t, data, r * width + c) : nlohmann::json(0.0));
        out.emplace_back(std::move(row));
    }
    return out;
}

inline nlohmann::json unpack(const nlohmann::json& j)
{
    return toArray(dtype(j), j.get_binary().data(), count(j), width(j));
}

/** Replaces every packed array in the document with its plain json array. */
inline void unpackAll(nlohmann::json& j)
{
    if (is(j))
        j = unpack(j);
    else if (j.is_array() || j.is_object())
        for (auto& child : j)
            unpackAll(child);
}
} // namespace lila::packed
