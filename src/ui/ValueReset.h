#pragma once

#include "ops/Command.h"
#include "scene/Plug.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lila::ui::valueReset
{
/** A middle press and release that moves less than this many pixels... */
inline constexpr int kMaxTravel = 4;
/** ...within this many milliseconds is a click (reset to default); anything else is a drag (scrub). */
inline constexpr int64_t kMaxMillis = 400;

/** Tells a middle click from a middle drag. Feed it the press, every move, and the release, in global pixels. */
class ClickTracker
{
public:
    void press(int x, int y, int64_t ms)
    {
        mDown = true;
        mX = x;
        mY = y;
        mStart = ms;
        mTravel = 0;
    }

    void move(int x, int y)
    {
        if (!mDown)
            return;
        int dx = x - mX, dy = y - mY;
        mTravel = std::max(mTravel, int(std::lround(std::sqrt(double(dx * dx + dy * dy)))));
    }

    /** The press has moved far enough to be a drag; scrubbing may start. */
    bool dragging() const { return mDown && mTravel >= kMaxTravel; }
    bool down() const { return mDown; }

    /** Ends the press; true when it was a click. */
    bool release(int x, int y, int64_t ms)
    {
        move(x, y);
        bool click = mDown && mTravel < kMaxTravel && ms - mStart < kMaxMillis;
        mDown = false;
        return click;
    }

    void cancel() { mDown = false; }

private:
    bool mDown = false;
    int mX = 0;
    int mY = 0;
    int64_t mStart = 0;
    int mTravel = 0;
};

/** The setAttr value restoring a plug (or one component of a Vec3) to its default; nullopt for kinds that don't reset. */
inline std::optional<json> plugDefault(const scene::AttrDef& def, int component = -1)
{
    const scene::PlugValue& v = def.defaultValue;
    auto number = [&]() -> std::optional<double> {
        if (auto* d = std::get_if<double>(&v))
            return *d;
        if (auto* i = std::get_if<int64_t>(&v))
            return double(*i);
        if (auto* b = std::get_if<bool>(&v))
            return *b ? 1.0 : 0.0;
        return std::nullopt;
    };
    switch (def.type)
    {
    case scene::PlugType::Bool:
        if (auto n = number())
            return json(*n != 0.0);
        return std::nullopt;
    case scene::PlugType::Int:
    case scene::PlugType::Enum:
        if (auto n = number())
            return json(int64_t(std::llround(*n)));
        return std::nullopt;
    case scene::PlugType::Float:
        if (auto n = number())
            return json(*n);
        return std::nullopt;
    case scene::PlugType::Vec3:
    {
        auto* v3 = std::get_if<glm::dvec3>(&v);
        if (!v3)
            return std::nullopt;
        if (component >= 0 && component < 3)
            return json((*v3)[component]);
        return json::array({v3->x, v3->y, v3->z});
    }
    case scene::PlugType::String:
        if (auto* s = std::get_if<std::string>(&v))
            return json(*s);
        return std::nullopt;
    default: return std::nullopt;
    }
}

/** A command flag's default, or nullopt when the flag has none (it is simply left out unless given). */
inline std::optional<json> flagDefault(const FlagDef& flag)
{
    if (flag.defaultValue.is_null())
        return std::nullopt;
    return std::optional<json>(flag.defaultValue);
}

/** "0.5", "12", "on", an enum's name, "1, 0, 0", "\"text\"" or "empty": how a default reads in a tooltip. */
inline std::string describe(const json& value, const std::vector<std::string>& enumNames = {})
{
    if (value.is_boolean())
        return value.get<bool>() ? "on" : "off";
    if (value.is_number_integer())
    {
        int64_t i = value.get<int64_t>();
        if (i >= 0 && i < int64_t(enumNames.size()))
            return enumNames[size_t(i)];
        return std::to_string(i);
    }
    if (value.is_number())
    {
        double d = value.get<double>();
        if (std::abs(d) < 5e-7)
            d = 0.0;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.4f", d);
        std::string s = buf;
        while (s.find('.') != std::string::npos && (s.back() == '0' || s.back() == '.'))
            s.pop_back();
        return s == "-0" ? "0" : s;
    }
    if (value.is_string())
        return value.get<std::string>().empty() ? "empty" : "\"" + value.get<std::string>() + "\"";
    if (value.is_array())
    {
        std::string out;
        for (const json& e : value)
            out += (out.empty() ? "" : ", ") + describe(e);
        return out;
    }
    return value.dump();
}
} // namespace lila::ui::valueReset
