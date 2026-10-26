#include "geo/Attribute.h"

#include <algorithm>

namespace lila::geo
{
size_t attrTypeSize(AttrType t)
{
    switch (t)
    {
    case AttrType::Float: return 4;
    case AttrType::Float2: return 8;
    case AttrType::Float3: return 12;
    case AttrType::Float4: return 16;
    case AttrType::Int: return 4;
    case AttrType::Bool: return 1;
    }
    return 0;
}

int attrTypeComponents(AttrType t)
{
    switch (t)
    {
    case AttrType::Float2: return 2;
    case AttrType::Float3: return 3;
    case AttrType::Float4: return 4;
    default: return 1;
    }
}

bool attrTypeIsFloat(AttrType t)
{
    return t == AttrType::Float || t == AttrType::Float2 || t == AttrType::Float3 || t == AttrType::Float4;
}

const char* attrTypeName(AttrType t)
{
    switch (t)
    {
    case AttrType::Float: return "float";
    case AttrType::Float2: return "float2";
    case AttrType::Float3: return "float3";
    case AttrType::Float4: return "float4";
    case AttrType::Int: return "int";
    case AttrType::Bool: return "bool";
    }
    return "?";
}

const char* domainName(Domain d)
{
    switch (d)
    {
    case Domain::Point: return "point";
    case Domain::Edge: return "edge";
    case Domain::Face: return "face";
    case Domain::Corner: return "corner";
    default: return "?";
    }
}

std::string attr::uvName(int set)
{
    return "uv" + std::to_string(set);
}

Attribute::Attribute(std::string name, AttrType type, size_t count, bool zeroed)
    : mName(std::move(name)), mType(type), mStride(attrTypeSize(type))
{
    if (zeroed)
        mData.assign(count * mStride, std::byte{0});
    else
        mData.resize(count * mStride);
}

void Attribute::resize(size_t n)
{
    const size_t old = size();
    mData.resize(n * mStride);
    if (n <= old)
        return;
    if (std::any_of(mDefault, mDefault + mStride, [](std::byte b) { return b != std::byte{0}; }))
        for (size_t i = old; i < n; ++i)
            reset(i);
    else
        std::memset(raw(old), 0, (n - old) * mStride);
}

void Attribute::copyFrom(const Attribute& other, size_t dst, size_t src)
{
    if (other.mType == mType)
        std::memcpy(raw(dst), other.raw(src), mStride);
}

void Attribute::mixFrom(const Attribute& other, size_t dst, std::span<const Index> srcs, std::span<const float> weights)
{
    if (other.mType != mType || srcs.empty())
        return;
    if (!attrTypeIsFloat(mType))
    {
        size_t best = 0;
        for (size_t i = 1; i < srcs.size(); ++i)
            if (weights[i] > weights[best])
                best = i;
        std::memcpy(raw(dst), other.raw(srcs[best]), mStride);
        return;
    }
    const int n = attrTypeComponents(mType);
    float acc[4] = {};
    for (size_t i = 0; i < srcs.size(); ++i)
    {
        const float* v = reinterpret_cast<const float*>(other.raw(srcs[i]));
        for (int k = 0; k < n; ++k)
            acc[k] += v[k] * weights[i];
    }
    std::memcpy(raw(dst), acc, mStride);
}

void Attribute::mix(size_t dst, std::span<const Index> srcs, std::span<const float> weights)
{
    if (srcs.empty())
        return;
    if (!attrTypeIsFloat(mType))
    {
        size_t best = 0;
        for (size_t i = 1; i < srcs.size(); ++i)
            if (weights[i] > weights[best])
                best = i;
        copy(dst, srcs[best]);
        return;
    }
    const int n = attrTypeComponents(mType);
    float acc[4] = {};
    for (size_t i = 0; i < srcs.size(); ++i)
    {
        const float* v = reinterpret_cast<const float*>(raw(srcs[i]));
        for (int k = 0; k < n; ++k)
            acc[k] += v[k] * weights[i];
    }
    std::memcpy(raw(dst), acc, mStride);
}

void Attribute::reorder(std::span<const Index> newToOld)
{
    UninitVector<std::byte> out(newToOld.size() * mStride);
    for (size_t i = 0; i < newToOld.size(); ++i)
        std::memcpy(out.data() + i * mStride, raw(newToOld[i]), mStride);
    mData = std::move(out);
}

Attribute* AttributeSet::find(std::string_view name)
{
    for (Attribute& a : mLayers)
        if (a.name() == name)
            return &a;
    return nullptr;
}

const Attribute* AttributeSet::find(std::string_view name) const
{
    for (const Attribute& a : mLayers)
        if (a.name() == name)
            return &a;
    return nullptr;
}

Attribute& AttributeSet::add(std::string_view name, AttrType type)
{
    if (Attribute* a = find(name))
    {
        if (a->type() == type)
            return *a;
        remove(name);
    }
    return mLayers.emplace_back(std::string(name), type, mSize);
}

Attribute& AttributeSet::addUnset(std::string_view name, AttrType type)
{
    if (Attribute* a = find(name); a && a->type() == type)
        return *a;
    remove(name);
    return mLayers.emplace_back(std::string(name), type, mSize, false);
}

bool AttributeSet::remove(std::string_view name)
{
    auto it = std::find_if(mLayers.begin(), mLayers.end(), [&](const Attribute& a) { return a.name() == name; });
    if (it == mLayers.end())
        return false;
    mLayers.erase(it);
    return true;
}

bool AttributeSet::rename(std::string_view from, std::string_view to)
{
    Attribute* a = find(from);
    if (!a || (from != to && find(to)))
        return false;
    a->setName(std::string(to));
    return true;
}

void AttributeSet::moveLayer(size_t from, size_t to)
{
    if (from >= mLayers.size() || to >= mLayers.size() || from == to)
        return;
    if (from < to)
        std::rotate(mLayers.begin() + from, mLayers.begin() + from + 1, mLayers.begin() + to + 1);
    else
        std::rotate(mLayers.begin() + to, mLayers.begin() + from, mLayers.begin() + from + 1);
}

std::vector<std::string> AttributeSet::names() const
{
    std::vector<std::string> out;
    for (const Attribute& a : mLayers)
        out.push_back(a.name());
    return out;
}

void AttributeSet::resize(size_t n)
{
    mSize = n;
    for (Attribute& a : mLayers)
        a.resize(n);
}

void AttributeSet::reset(size_t i)
{
    for (Attribute& a : mLayers)
        a.reset(i);
}

void AttributeSet::copy(size_t dst, size_t src)
{
    for (Attribute& a : mLayers)
        a.copy(dst, src);
}

void AttributeSet::mix(size_t dst, std::span<const Index> srcs, std::span<const float> weights)
{
    for (Attribute& a : mLayers)
        a.mix(dst, srcs, weights);
}

void AttributeSet::reorder(std::span<const Index> newToOld)
{
    mSize = newToOld.size();
    for (Attribute& a : mLayers)
        a.reorder(newToOld);
}

void AttributeSet::clear()
{
    mLayers.clear();
    mSize = 0;
}

void AttributeSet::copyFrom(const AttributeSet& other, size_t dst, size_t src)
{
    for (size_t i = 0; i < mLayers.size(); ++i)
    {
        const Attribute* o = i < other.mLayers.size() && other.mLayers[i].name() == mLayers[i].name()
            ? &other.mLayers[i]
            : other.find(mLayers[i].name());
        if (o)
            mLayers[i].copyFrom(*o, dst, src);
    }
}

void AttributeSet::mixFrom(const AttributeSet& other, size_t dst, std::span<const Index> srcs,
                           std::span<const float> weights)
{
    for (size_t i = 0; i < mLayers.size(); ++i)
    {
        // Same index first: sets built with mergeLayout line up.
        const Attribute* o = i < other.mLayers.size() && other.mLayers[i].name() == mLayers[i].name()
            ? &other.mLayers[i]
            : other.find(mLayers[i].name());
        if (o)
            mLayers[i].mixFrom(*o, dst, srcs, weights);
    }
}

void AttributeSet::mergeLayout(const AttributeSet& other)
{
    for (const Attribute& o : other.mLayers)
    {
        Attribute* a = find(o.name());
        if (a && a->type() == o.type())
            continue;
        Attribute& added = add(o.name(), o.type());
        added.setDefaultRaw(o.defaultRaw());
        for (size_t i = 0; i < mSize; ++i)
            added.reset(i);
    }
}
} // namespace lila::geo
