#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lila::geo
{
using Index = uint32_t;
inline constexpr Index kInvalid = ~Index(0);

enum class Domain : uint8_t
{
    Point,
    Edge,
    Face,
    Corner,
    Count
};

enum class AttrType : uint8_t
{
    Float,
    Float2,
    Float3,
    Float4,
    Int,
    Bool
};

size_t attrTypeSize(AttrType t);
int attrTypeComponents(AttrType t);
bool attrTypeIsFloat(AttrType t);
const char* attrTypeName(AttrType t);
const char* domainName(Domain d);

template <class T> struct AttrTypeOf;
template <> struct AttrTypeOf<float> { static constexpr AttrType value = AttrType::Float; };
template <> struct AttrTypeOf<glm::vec2> { static constexpr AttrType value = AttrType::Float2; };
template <> struct AttrTypeOf<glm::vec3> { static constexpr AttrType value = AttrType::Float3; };
template <> struct AttrTypeOf<glm::vec4> { static constexpr AttrType value = AttrType::Float4; };
template <> struct AttrTypeOf<int32_t> { static constexpr AttrType value = AttrType::Int; };
template <> struct AttrTypeOf<bool> { static constexpr AttrType value = AttrType::Bool; };

/** Built-in attribute names. */
namespace attr
{
inline constexpr std::string_view kNormal = "normal";       // corner float3, custom split normals
inline constexpr std::string_view kTangent = "tangent";     // corner float4, w is the bitangent sign
inline constexpr std::string_view kColor = "color";         // point or corner float4
inline constexpr std::string_view kHard = "hard";           // edge bool
inline constexpr std::string_view kSeam = "seam";           // edge bool
inline constexpr std::string_view kCrease = "crease";       // edge float, 0 smooth to 1+ sharp
inline constexpr std::string_view kBevelWeight = "bevelWeight"; // edge float, 0 to 1, read by bevel modifiers
inline constexpr std::string_view kMaterial = "material";   // face int, index into Mesh::materialNames()
inline constexpr std::string_view kSmoothGroup = "smoothGroup"; // face int, 0 means faceted

/** "uv0", "uv1", ... corner float2. */
std::string uvName(int set);
} // namespace attr

/** One named, typed value per element of a domain. Data is type-erased; access it through values<T>(). */
class Attribute
{
public:
    Attribute(std::string name, AttrType type, size_t count);

    const std::string& name() const { return mName; }
    void setName(std::string name) { mName = std::move(name); }
    AttrType type() const { return mType; }
    size_t size() const { return mStride ? mData.size() / mStride : 0; }
    size_t stride() const { return mStride; }

    template <class T> std::span<T> values()
    {
        static_assert(sizeof(T) <= 16);
        if (AttrTypeOf<T>::value != mType)
            return {};
        return {reinterpret_cast<T*>(mData.data()), size()};
    }
    template <class T> std::span<const T> values() const
    {
        if (AttrTypeOf<T>::value != mType)
            return {};
        return {reinterpret_cast<const T*>(mData.data()), size()};
    }

    std::byte* raw(size_t i) { return mData.data() + i * mStride; }
    const std::byte* raw(size_t i) const { return mData.data() + i * mStride; }

    /** Value new elements get. Zero unless set. */
    template <class T> void setDefault(const T& v)
    {
        if (AttrTypeOf<T>::value == mType)
            std::memcpy(mDefault, &v, sizeof(T));
    }

    const std::byte* defaultRaw() const { return mDefault; }
    void setDefaultRaw(const std::byte* v) { std::memcpy(mDefault, v, mStride); }

    void resize(size_t n);
    void reset(size_t i) { std::memcpy(raw(i), mDefault, mStride); }
    void copy(size_t dst, size_t src) { std::memcpy(raw(dst), raw(src), mStride); }
    void copyFrom(const Attribute& other, size_t dst, size_t src);
    void mixFrom(const Attribute& other, size_t dst, std::span<const Index> srcs, std::span<const float> weights);

    /** Weighted blend of elements into dst. Ints and bools take the heaviest source. */
    void mix(size_t dst, std::span<const Index> srcs, std::span<const float> weights);

    /** Keeps element newToOld[i] at position i. */
    void reorder(std::span<const Index> newToOld);

private:
    std::string mName;
    AttrType mType;
    size_t mStride;
    alignas(16) std::byte mDefault[16] = {};
    std::vector<std::byte> mData;
};

/** All attributes of one domain. Element count is kept in sync by the Mesh. */
class AttributeSet
{
public:
    Attribute* find(std::string_view name);
    const Attribute* find(std::string_view name) const;

    /** Returns the existing layer when the type matches; a layer of another type is replaced. */
    Attribute& add(std::string_view name, AttrType type);
    bool remove(std::string_view name);
    /** Renames in place, keeping the layer's position. False when from is missing or to is taken. */
    bool rename(std::string_view from, std::string_view to);
    /** Moves layer from to position to, shifting the layers between. */
    void moveLayer(size_t from, size_t to);
    bool has(std::string_view name) const { return find(name) != nullptr; }

    /** Empty when missing or of another type. Spans are invalidated when elements are added. */
    template <class T> std::span<T> get(std::string_view name)
    {
        Attribute* a = find(name);
        return a ? a->values<T>() : std::span<T>{};
    }
    template <class T> std::span<const T> get(std::string_view name) const
    {
        const Attribute* a = find(name);
        return a ? a->values<T>() : std::span<const T>{};
    }
    template <class T> std::span<T> getOrAdd(std::string_view name)
    {
        return add(name, AttrTypeOf<T>::value).template values<T>();
    }

    size_t size() const { return mSize; }
    size_t layerCount() const { return mLayers.size(); }
    Attribute& layer(size_t i) { return mLayers[i]; }
    const Attribute& layer(size_t i) const { return mLayers[i]; }
    std::vector<std::string> names() const;

    void resize(size_t n);
    void reset(size_t i);
    void copy(size_t dst, size_t src);
    void mix(size_t dst, std::span<const Index> srcs, std::span<const float> weights);
    void reorder(std::span<const Index> newToOld);
    void clear();

    /** Copies element src of other into dst for every layer the two share by name and type. */
    void copyFrom(const AttributeSet& other, size_t dst, size_t src);

    /** Weighted blend of other's elements into dst, for layers shared by name and type. */
    void mixFrom(const AttributeSet& other, size_t dst, std::span<const Index> srcs, std::span<const float> weights);

    /** Adds any layer of other that this set lacks. */
    void mergeLayout(const AttributeSet& other);

private:
    std::vector<Attribute> mLayers;
    size_t mSize = 0;
};
} // namespace lila::geo
