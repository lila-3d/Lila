#pragma once

#include "core/Export.h"

#include "math/Color.h"
#include "math/Quat.h"
#include "math/Rotator.h"
#include "math/Transform.h"
#include "math/Vector.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ext
{
class Context;
}

/**
 * Debug drawing, after Unreal's DrawDebugHelpers: lines, boxes, spheres, arrows, text... in world space, shown in every
 * viewport. Safe from any thread, and cheap enough to call in loops.
 *
 * Every draw has a lifetime, so a forgotten call clears itself:
 *
 *     debugDraw::line(a, b);                                                   // 5 s (the debugDraw/defaultDuration pref)
 *     debugDraw::sphere(c, 50.0, 16, {.color = math::LinearColor::red(), .duration = 0}); // the next redraw only
 *
 * Draws that stay need a channel, which has an owner and is cleared with it:
 *
 *     debugDraw::Channel ch = debugDraw::channel(ctx, "paths");               // removed when the extension unloads
 *     debugDraw::lines(points, {.persistent = true, .channel = ch});
 *     ch.clear();                                                             // the caller's job
 *
 * Every channel is also cleared by File > New and Open, and by Show > Clear Debug Drawing.
 *
 * Bad input never draws: non-finite or absurdly far positions and sizes are dropped with a warning, segment counts and
 * thickness are clamped. Past the debugDraw/maxPrimitives pref (1M lines and triangles together; points and texts have
 * caps of their own) the oldest draws go first, with one warning.
 */
namespace lila::debugDraw
{
/** Style::duration's default: the debugDraw/defaultDuration pref. */
inline constexpr double kDefaultDuration = -1.0;
/** Positions and sizes beyond this (cm, 10,000 km) are taken for garbage and dropped. */
inline constexpr double kMaxCoordinate = 1.0e9;
inline constexpr int kMinSegments = 3;
inline constexpr int kMaxSegments = 256;
inline constexpr float kMaxThickness = 32.0f;
inline constexpr size_t kMaxTextLength = 1024;

/** Draws into a channel: Lila clears them with it. A default Channel is none: draws go to the transient pool. */
class LILA_FOUNDATION_API Channel
{
public:
    Channel() = default;
    explicit Channel(uint32_t id) : mId(id) {}

    uint32_t id() const { return mId; }
    /** False for the default Channel and once the channel was removed. */
    bool valid() const;
    std::string name() const;
    std::string owner() const;
    /** Removes everything drawn into it; the channel stays usable. */
    void clear() const;
    /** Clears it and retires its id: later draws into it are dropped. */
    void remove() const;
    void setVisible(bool visible) const;
    bool visible() const;
    /** Lines, triangles, points and texts it holds now. */
    size_t primitiveCount() const;

    explicit operator bool() const { return valid(); }
    bool operator==(const Channel&) const = default;

private:
    uint32_t mId = 0;
};

struct Style
{
    math::LinearColor color = math::LinearColor::white();
    /** Seconds on screen. kDefaultDuration: the debugDraw/defaultDuration pref; 0: the next redraw only. */
    double duration = kDefaultDuration;
    /** Line width in pixels; 0 is a hairline. */
    float thickness = 0.0f;
    /** Drawn over every surface instead of depth tested. */
    bool onTop = false;
    /** Stays until its channel is cleared; needs a channel (ignored, with a warning, without one). */
    bool persistent = false;
    Channel channel;
};

/** The channel called name for owner (made on first use). An empty owner is the extension being loaded, else Lila. */
LILA_FOUNDATION_API Channel channel(std::string_view name, std::string owner = {});
/** Owned by the extension: removed, with what it holds, when it unloads. */
LILA_FOUNDATION_API Channel channel(ext::Context& ctx, std::string_view name);
LILA_FOUNDATION_API std::vector<Channel> channels();

// Shapes. Positions are world space (cm), rotations Lila's (math::Rotator is Unreal's meaning on Lila's axes).
LILA_FOUNDATION_API void point(const math::Vector& position, float size = 8.0f, const Style& style = {});
LILA_FOUNDATION_API void line(const math::Vector& start, const math::Vector& end, const Style& style = {});
/** A polyline through points, closed back to the first when asked. */
LILA_FOUNDATION_API void lines(std::span<const math::Vector> points, bool closed = false, const Style& style = {});
/** Independent segments: points 0-1, 2-3, ... */
LILA_FOUNDATION_API void segments(std::span<const math::Vector> points, const Style& style = {});
/** A line with an arrow head at end, arrowSize long. */
LILA_FOUNDATION_API void arrow(const math::Vector& start, const math::Vector& end, double arrowSize = 10.0, const Style& style = {});
/** extent is half the size. */
LILA_FOUNDATION_API void box(const math::Vector& center, const math::Vector& extent, const math::Quat& rotation = {}, const Style& style = {});
LILA_FOUNDATION_API void solidBox(const math::Vector& center, const math::Vector& extent, const math::Quat& rotation = {}, const Style& style = {});
LILA_FOUNDATION_API void sphere(const math::Vector& center, double radius, int segments = 16, const Style& style = {});
/** halfHeight runs from the center to a cap's tip, along the rotated up axis. */
LILA_FOUNDATION_API void capsule(const math::Vector& center, double halfHeight, double radius, const math::Quat& rotation = {}, const Style& style = {});
LILA_FOUNDATION_API void cylinder(const math::Vector& start, const math::Vector& end, double radius, int segments = 16, const Style& style = {});
/** In the plane facing normal (up when zero). */
LILA_FOUNDATION_API void circle(const math::Vector& center, double radius, int segments = 32, const math::Vector& normal = {},
                                const Style& style = {});
/** From origin along direction, length long, opening angleWidth / angleHeight degrees off the axis. */
LILA_FOUNDATION_API void cone(const math::Vector& origin, const math::Vector& direction, double length, double angleWidth, double angleHeight,
                              int sides = 16, const Style& style = {});
/** Lila's X, Y and Z axes, rotated, scale long, red, green and blue (style.color is ignored). */
LILA_FOUNDATION_API void coordinateSystem(const math::Vector& origin, const math::Quat& rotation = {}, double scale = 50.0, const Style& style = {});
/** A camera's view volume: transform places a camera looking down its forward axis. fov is horizontal, in degrees. */
LILA_FOUNDATION_API void frustum(const math::Transform& transform, double fovDegrees, double aspect, double nearDist, double farDist,
                                 const Style& style = {});
/** A filled square through point, facing normal, extent half wide. */
LILA_FOUNDATION_API void solidPlane(const math::Vector& point, const math::Vector& normal, double extent, const Style& style = {});
/** Text at a world position, facing the screen. */
LILA_FOUNDATION_API void text(const math::Vector& position, std::string_view text, const Style& style = {});

/** Clears channel, or everything (every channel and the transient pool) when it is the default Channel. */
LILA_FOUNDATION_API void flush(Channel channel = {});

struct Limits
{
    /** Lines and triangles together; past it the oldest go first. */
    size_t maxPrimitives = 1'000'000;
    /** Points and texts draw as screen overlays, so they have their own, smaller caps. */
    size_t maxPoints = 100'000;
    size_t maxTexts = 10'000;
    double defaultDuration = 5.0;
    /** Cap on durations of draws that are not persistent. */
    double maxDuration = 3600.0;
};
/** The general prefs' limits (/debugDraw/maxPrimitives, maxPoints, maxTexts, defaultDuration, maxDuration), read at most once a second. */
LILA_FOUNDATION_API Limits limits();
/** Overrides the prefs until reset with nullptr. For tests. */
LILA_FOUNDATION_API void setLimits(const Limits* limits);

struct Stats
{
    size_t lines = 0;
    size_t triangles = 0;
    size_t points = 0;
    size_t texts = 0;
    size_t channels = 0;
    /** Dropped for the cap since the last flush of everything. */
    size_t dropped = 0;
    /** Rejected as invalid input since start. */
    size_t rejected = 0;
};
LILA_FOUNDATION_API Stats stats();

// For viewports. Times are seconds on a steady clock (now()).

struct Vertex
{
    glm::vec3 pos;
    glm::vec4 color;
};
struct LineBatch
{
    float width = 1.0f;
    bool onTop = false;
    std::vector<Vertex> vertices; // pairs
};
struct ScreenPoint
{
    glm::vec3 pos;
    glm::vec4 color;
    float size = 8.0f;
};
struct ScreenText
{
    glm::vec3 pos;
    glm::vec4 color;
    std::string text;
};
/** What is visible now, by kind. Immutable once made. */
struct Snapshot
{
    uint64_t generation = 0;
    std::vector<LineBatch> lines; // one per width and depth mode
    std::vector<Vertex> triangles;
    std::vector<ScreenPoint> points;
    std::vector<ScreenText> texts;
    bool empty() const { return lines.empty() && triangles.empty() && points.empty() && texts.empty(); }
};

LILA_FOUNDATION_API double now();
/**
 * Drops what expired by `time` and returns what is left, reused while nothing changed. Draws for the next redraw only
 * stay for every view drawn in the next few milliseconds, then go.
 */
LILA_FOUNDATION_API std::shared_ptr<const Snapshot> snapshot(double time = now());
/** When the next draw expires (a redraw is due then); infinity for never. */
LILA_FOUNDATION_API double nextExpiry();
/** Counts every change. */
LILA_FOUNDATION_API uint64_t generation();
/** Called on the submitting thread (any) after a change, so the host can schedule a redraw. One hook; empty clears it. */
LILA_FOUNDATION_API void setChangedHook(std::function<void()> hook);
} // namespace lila::debugDraw
