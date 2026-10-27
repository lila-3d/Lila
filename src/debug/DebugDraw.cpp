#include "debug/DebugDraw.h"

#include "core/Events.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Registry.h"
#include "math/MathCore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>

namespace lila::debugDraw
{
namespace
{
using math::Vector;

constexpr double kInf = std::numeric_limits<double>::infinity();
/** A redraw-only draw stays this long after the first view drew it, so every view redrawn in the same pass shows it. */
constexpr double kFrameGrace = 0.05;
/** ...and this long after it was made when no view ever draws it (batch mode, hidden views). */
constexpr double kFrameMaxWait = 2.0;
constexpr double kWarnInterval = 10.0;

enum Flags : uint8_t
{
    OnTop = 1,
    Frame = 2, // for the next redraw only; expiry not yet settled
};

struct Meta
{
    double expiry = kInf;
    uint64_t seq = 0;
    uint32_t channel = 0;
    uint8_t flags = 0;
};

struct Seg
{
    glm::vec3 a, b;
    glm::vec4 color;
    float width;
    Meta m;
};
struct Tri
{
    glm::vec3 p[3];
    glm::vec4 color;
    Meta m;
};
struct Pt
{
    glm::vec3 pos;
    glm::vec4 color;
    float size;
    Meta m;
};
struct Txt
{
    glm::vec3 pos;
    glm::vec4 color;
    std::string text;
    Meta m;
};

struct ChannelInfo
{
    std::string name;
    std::string owner;
    bool visible = true;
};

struct Store
{
    std::mutex mutex;
    std::deque<Seg> segs;
    std::deque<Tri> tris;
    std::deque<Pt> pts;
    std::deque<Txt> txts;
    std::map<uint32_t, ChannelInfo> channels;
    std::map<std::pair<std::string, std::string>, uint32_t> byName; // (owner, name)
    uint32_t nextChannel = 1;
    uint64_t seq = 0;
    uint64_t generation = 1;
    double nextExpiry = kInf;
    size_t pendingFrame = 0;
    size_t dropped = 0;
    bool capWarned = false;
    std::shared_ptr<const Snapshot> cached;
    std::shared_ptr<const std::function<void()>> hook;

    size_t primitives() const { return segs.size() + tris.size(); }
};

Store& store()
{
    // Never destroyed: worker threads may still draw while the process exits.
    static Store* s = new Store;
    return *s;
}

std::atomic<size_t> gRejected{0};

// ---------------------------------------------------------------- warnings

/** Logs the first time, then at most every kWarnInterval seconds per key with how many were held back. */
void warnLimited(const std::string& key, const std::string& text)
{
    struct Seen
    {
        double last = -kInf;
        size_t held = 0;
    };
    static std::mutex m;
    static std::map<std::string, Seen> seen;
    const double t = now();
    std::string line;
    {
        std::lock_guard lock(m);
        Seen& s = seen[key];
        if (t - s.last < kWarnInterval)
        {
            ++s.held;
            return;
        }
        line = s.held ? std::format("debug draw: {} ({} more like it since the last warning)", text, s.held) : "debug draw: " + text;
        s.last = t;
        s.held = 0;
    }
    log::warn("{}", line);
}

bool reject(const char* what, const char* why)
{
    ++gRejected;
    warnLimited(std::string(what) + why, std::format("{}: {}; nothing drawn", what, why));
    return false;
}

bool finite(const Vector& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool validPoint(const char* what, const Vector& v)
{
    if (!finite(v))
        return reject(what, "a position is NaN or infinite");
    if (std::abs(v.x) > kMaxCoordinate || std::abs(v.y) > kMaxCoordinate || std::abs(v.z) > kMaxCoordinate)
        return reject(what, "a position is further than 10,000 km from the origin");
    return true;
}

bool validSize(const char* what, double size)
{
    if (!std::isfinite(size))
        return reject(what, "a size is NaN or infinite");
    if (std::abs(size) > kMaxCoordinate)
        return reject(what, "a size is over 10,000 km");
    return true;
}

bool validSizes(const char* what, const Vector& v)
{
    return validSize(what, v.x) && validSize(what, v.y) && validSize(what, v.z);
}

bool validDirection(const char* what, const Vector& v)
{
    return finite(v) || reject(what, "a direction is NaN or infinite");
}

bool validRotation(const char* what, const math::Quat& q)
{
    if (q.containsNaN() || !std::isfinite(q.sizeSquared()) || q.sizeSquared() < 1.0e-12)
        return reject(what, "the rotation is NaN or zero");
    return true;
}

int clampSegments(const char* what, int n, int lo = kMinSegments, int hi = kMaxSegments)
{
    if (n >= lo && n <= hi)
        return n;
    int c = std::clamp(n, lo, hi);
    warnLimited(what, std::format("{}: {} segments clamped to {}", what, n, c));
    return c;
}

// ---------------------------------------------------------------- limits

std::mutex gLimitsMutex;
Limits gLimits;
double gLimitsRead = -kInf;
bool gLimitsOverridden = false;

// ---------------------------------------------------------------- building

/** One call's geometry, made on the caller's thread before the store is locked. */
struct Batch
{
    std::vector<glm::vec3> segs; // pairs
    std::vector<glm::vec3> tris; // triples
    std::vector<glm::vec4> segColors; // per segment when not empty
    /** Segments the caller left out for the cap. */
    size_t skipped = 0;
    void clear()
    {
        segs.clear();
        tris.clear();
        segColors.clear();
        skipped = 0;
        // One huge call must not pin its memory to the thread for good.
        if (segs.capacity() > 65536)
            segs.shrink_to_fit();
        if (tris.capacity() > 65536)
            tris.shrink_to_fit();
    }
    void line(const Vector& a, const Vector& b)
    {
        segs.push_back(glm::vec3(a.toGlm()));
        segs.push_back(glm::vec3(b.toGlm()));
    }
    void tri(const Vector& a, const Vector& b, const Vector& c)
    {
        tris.push_back(glm::vec3(a.toGlm()));
        tris.push_back(glm::vec3(b.toGlm()));
        tris.push_back(glm::vec3(c.toGlm()));
    }
    void ring(const Vector& center, const Vector& u, const Vector& v, double radius, int n)
    {
        Vector prev = center + u * radius;
        for (int i = 1; i <= n; ++i)
        {
            const double a = 2.0 * math::kPi * double(i) / double(n);
            Vector p = center + (u * std::cos(a) + v * std::sin(a)) * radius;
            line(prev, p);
            prev = p;
        }
    }
    /** Half a ring, from +u through +v to -u. */
    void arc(const Vector& center, const Vector& u, const Vector& v, double radius, int n)
    {
        Vector prev = center + u * radius;
        for (int i = 1; i <= n; ++i)
        {
            const double a = math::kPi * double(i) / double(n);
            Vector p = center + (u * std::cos(a) + v * std::sin(a)) * radius;
            line(prev, p);
            prev = p;
        }
    }
};

Batch& scratch()
{
    thread_local Batch b;
    b.clear();
    return b;
}

/** Two unit vectors perpendicular to n and each other. */
void basis(const Vector& n, Vector& u, Vector& v)
{
    const Vector a = std::abs(n.x) < 0.9 ? Vector(1.0, 0.0, 0.0) : Vector(0.0, 1.0, 0.0);
    u = (a ^ n).getSafeNormal();
    v = n ^ u;
}

Vector unitOr(const Vector& v, const Vector& fallback)
{
    const double len = v.size();
    return len > 1.0e-12 && std::isfinite(len) ? v / len : fallback;
}

glm::vec4 sanitizeColor(const char* what, const math::LinearColor& c)
{
    glm::vec4 out(c.r, c.g, c.b, c.a);
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(out[i]))
        {
            warnLimited("color", std::format("{}: the color is NaN or infinite; drawn white", what));
            return glm::vec4(1.0f);
        }
    out = glm::max(out, glm::vec4(0.0f));
    out.a = std::min(out.a, 1.0f);
    return out;
}

/** The style resolved into what the store keeps; false to drop the draw. */
struct Resolved
{
    glm::vec4 color{1.0f};
    float width = 1.0f;
    Meta meta;
};

bool resolve(const char* what, const Style& style, double time, const Limits& lim, Resolved& out)
{
    out.color = sanitizeColor(what, style.color);
    float t = style.thickness;
    if (!std::isfinite(t) || t < 0.0f)
        t = 0.0f;
    if (t > kMaxThickness)
    {
        warnLimited("thickness", std::format("{}: thickness {} clamped to {}", what, t, kMaxThickness));
        t = kMaxThickness;
    }
    // Half-pixel steps keep the number of width batches small.
    out.width = std::max(1.0f, std::round(t * 2.0f) * 0.5f);
    out.meta.channel = style.channel.id();
    out.meta.flags = style.onTop ? OnTop : 0;

    bool persistent = style.persistent;
    if (persistent && out.meta.channel == 0)
    {
        warnLimited("persistent", std::format("{}: persistent draws need a channel (debugDraw::channel); drawn with the default duration", what));
        persistent = false;
    }
    if (persistent)
    {
        out.meta.expiry = kInf;
        return true;
    }
    double d = style.duration;
    if (std::isnan(d) || d < 0.0)
        d = lim.defaultDuration;
    if (d == 0.0)
    {
        out.meta.flags |= Frame;
        out.meta.expiry = time + kFrameMaxWait;
        return true;
    }
    if (d > lim.maxDuration)
    {
        warnLimited("duration", std::format("{}: duration {} s clamped to {} s; use persistent with a channel for draws that stay", what,
                                            d, lim.maxDuration));
        d = lim.maxDuration;
    }
    out.meta.expiry = time + d;
    return true;
}

void notify(Store& s, std::unique_lock<std::mutex>& lock)
{
    ++s.generation;
    std::shared_ptr<const std::function<void()>> hook = s.hook;
    lock.unlock();
    if (hook && *hook)
        (*hook)();
}

/** Drops the oldest primitives until the cap holds, counting `skipped` the caller dropped already. Store locked. */
void enforceCap(Store& s, const Limits& lim, size_t skipped = 0)
{
    size_t over = s.primitives() > lim.maxPrimitives ? s.primitives() - lim.maxPrimitives : 0;
    const size_t droppedBefore = s.dropped;
    s.dropped += skipped;
    auto popFront = [&s](auto& d) {
        s.pendingFrame -= (d.front().m.flags & Frame) ? 1 : 0;
        d.pop_front();
        ++s.dropped;
    };
    while (over > 0)
    {
        const bool seg = !s.segs.empty() && (s.tris.empty() || s.segs.front().m.seq < s.tris.front().m.seq);
        if (seg)
            popFront(s.segs);
        else
            popFront(s.tris);
        --over;
    }
    while (s.pts.size() > lim.maxPoints)
        popFront(s.pts);
    while (s.txts.size() > lim.maxTexts)
        popFront(s.txts);
    if (s.dropped > droppedBefore && !s.capWarned)
    {
        s.capWarned = true;
        log::warn("debug draw: over the limit of {} lines and triangles, {} points or {} texts; the oldest are dropped. Something "
                  "draws every frame without a short duration, or never clears its channel. Raise debugDraw/maxPrimitives (or "
                  "maxPoints, maxTexts) in the general prefs if you need more.",
                  lim.maxPrimitives, lim.maxPoints, lim.maxTexts);
    }
}

bool channelUsable(Store& s, uint32_t id, const char* what)
{
    if (id == 0 || s.channels.contains(id))
        return true;
    warnLimited("deadChannel", std::format("{}: the channel was removed; nothing drawn", what));
    return false;
}

void commit(const char* what, const Batch& b, const Style& style)
{
    if (b.segs.empty() && b.tris.empty())
        return;
    const Limits lim = limits();
    const double time = now();
    Resolved r;
    if (!resolve(what, style, time, lim, r))
        return;
    size_t segCount = b.segs.size() / 2;
    size_t triCount = b.tris.size() / 3;
    // A single call bigger than the cap keeps its newest part, like the cap would.
    size_t skipSegs = 0, skipTris = 0;
    if (segCount + triCount > lim.maxPrimitives)
    {
        skipTris = std::min(triCount, segCount + triCount - lim.maxPrimitives);
        skipSegs = std::min(segCount, segCount + triCount - lim.maxPrimitives - skipTris);
    }

    Store& s = store();
    std::unique_lock lock(s.mutex);
    if (!channelUsable(s, r.meta.channel, what))
        return;
    for (size_t i = skipSegs; i < segCount; ++i)
    {
        Seg seg{b.segs[2 * i], b.segs[2 * i + 1], b.segColors.empty() ? r.color : b.segColors[i] * glm::vec4(1, 1, 1, r.color.a), r.width, r.meta};
        seg.m.seq = ++s.seq;
        s.segs.push_back(seg);
    }
    for (size_t i = skipTris; i < triCount; ++i)
    {
        Tri tri{{b.tris[3 * i], b.tris[3 * i + 1], b.tris[3 * i + 2]}, r.color, r.meta};
        tri.m.seq = ++s.seq;
        s.tris.push_back(tri);
    }
    if (r.meta.flags & Frame)
        s.pendingFrame += segCount - skipSegs + triCount - skipTris;
    s.nextExpiry = std::min(s.nextExpiry, r.meta.expiry);
    enforceCap(s, lim, skipSegs + skipTris + b.skipped);
    notify(s, lock);
}

template <class D, class F> size_t eraseIf(std::deque<D>& d, size_t& pendingFrame, F&& f)
{
    size_t n = 0;
    std::erase_if(d, [&](const D& e) {
        if (!f(e))
            return false;
        pendingFrame -= (e.m.flags & Frame) ? 1 : 0;
        ++n;
        return true;
    });
    return n;
}

/** Store locked. */
size_t eraseChannel(Store& s, uint32_t id)
{
    auto in = [id](const auto& e) { return e.m.channel == id; };
    return eraseIf(s.segs, s.pendingFrame, in) + eraseIf(s.tris, s.pendingFrame, in) + eraseIf(s.pts, s.pendingFrame, in) +
           eraseIf(s.txts, s.pendingFrame, in);
}

void clearAll(Store& s)
{
    s.segs.clear();
    s.tris.clear();
    s.pts.clear();
    s.txts.clear();
    s.pendingFrame = 0;
    s.dropped = 0;
    s.capWarned = false;
    s.nextExpiry = kInf;
}

bool removeChannel(uint32_t id)
{
    Store& s = store();
    std::unique_lock lock(s.mutex);
    auto it = s.channels.find(id);
    if (it == s.channels.end())
        return false;
    eraseChannel(s, id);
    s.byName.erase({it->second.owner, it->second.name});
    s.channels.erase(it);
    notify(s, lock);
    return true;
}

size_t removeOwnedChannels(std::string_view owner)
{
    std::vector<uint32_t> ids;
    {
        Store& s = store();
        std::lock_guard lock(s.mutex);
        for (const auto& [id, info] : s.channels)
            if (info.owner == owner)
                ids.push_back(id);
    }
    for (uint32_t id : ids)
        removeChannel(id);
    return ids.size();
}

/** Lila clears every channel with the scene, and removes an extension's with it. */
struct Install
{
    Install()
    {
        owners::track([](std::string_view owner) { return removeOwnedChannels(owner); });
        auto clear = [](const events::EventData&) { flush(); };
        events::subscribe(events::Event::SceneNew, clear);
        events::subscribe(events::Event::SceneOpened, clear);
    }
};
const Install gInstall;
} // namespace

// ---------------------------------------------------------------- channels

bool Channel::valid() const
{
    if (!mId)
        return false;
    Store& s = store();
    std::lock_guard lock(s.mutex);
    return s.channels.contains(mId);
}

std::string Channel::name() const
{
    Store& s = store();
    std::lock_guard lock(s.mutex);
    auto it = s.channels.find(mId);
    return it == s.channels.end() ? std::string() : it->second.name;
}

std::string Channel::owner() const
{
    Store& s = store();
    std::lock_guard lock(s.mutex);
    auto it = s.channels.find(mId);
    return it == s.channels.end() ? std::string() : it->second.owner;
}

void Channel::clear() const
{
    if (mId)
        flush(*this);
}

void Channel::remove() const
{
    if (mId)
        removeChannel(mId);
}

void Channel::setVisible(bool visible) const
{
    Store& s = store();
    std::unique_lock lock(s.mutex);
    auto it = s.channels.find(mId);
    if (it == s.channels.end() || it->second.visible == visible)
        return;
    it->second.visible = visible;
    notify(s, lock);
}

bool Channel::visible() const
{
    Store& s = store();
    std::lock_guard lock(s.mutex);
    auto it = s.channels.find(mId);
    return it != s.channels.end() && it->second.visible;
}

size_t Channel::primitiveCount() const
{
    Store& s = store();
    std::lock_guard lock(s.mutex);
    if (!mId || !s.channels.contains(mId))
        return 0;
    auto in = [this](const auto& e) { return e.m.channel == mId; };
    return size_t(std::count_if(s.segs.begin(), s.segs.end(), in) + std::count_if(s.tris.begin(), s.tris.end(), in) +
                  std::count_if(s.pts.begin(), s.pts.end(), in) + std::count_if(s.txts.begin(), s.txts.end(), in));
}

Channel channel(std::string_view name, std::string owner)
{
    if (owner.empty())
        owner = owners::attributedOwner() ? *owners::attributedOwner() : std::string("lila");
    Store& s = store();
    std::unique_lock lock(s.mutex);
    auto key = std::make_pair(owner, std::string(name));
    if (auto it = s.byName.find(key); it != s.byName.end())
        return Channel(it->second);
    const uint32_t id = s.nextChannel++;
    s.channels[id] = {std::string(name), std::move(owner)};
    s.byName[std::move(key)] = id;
    return Channel(id);
}

Channel channel(ext::Context& ctx, std::string_view name)
{
    bool existed = false;
    {
        Store& s = store();
        std::lock_guard lock(s.mutex);
        existed = s.byName.contains({ctx.owner(), std::string(name)});
    }
    Channel ch = channel(name, ctx.owner());
    if (!existed)
        ctx.track("debugDraw:" + std::string(name), [ch] { ch.remove(); });
    return ch;
}

std::vector<Channel> channels()
{
    Store& s = store();
    std::lock_guard lock(s.mutex);
    std::vector<Channel> out;
    for (const auto& [id, info] : s.channels)
        out.emplace_back(id);
    return out;
}

// ---------------------------------------------------------------- shapes

void point(const Vector& position, float size, const Style& style)
{
    if (!validPoint("point", position))
        return;
    if (!std::isfinite(size) || size <= 0.0f)
        size = 8.0f;
    size = std::min(size, 64.0f);
    const Limits lim = limits();
    const double time = now();
    Resolved r;
    if (!resolve("point", style, time, lim, r))
        return;
    Store& s = store();
    std::unique_lock lock(s.mutex);
    if (!channelUsable(s, r.meta.channel, "point"))
        return;
    Pt p{glm::vec3(position.toGlm()), r.color, size, r.meta};
    p.m.seq = ++s.seq;
    s.pts.push_back(p);
    s.pendingFrame += (r.meta.flags & Frame) ? 1 : 0;
    s.nextExpiry = std::min(s.nextExpiry, r.meta.expiry);
    enforceCap(s, lim);
    notify(s, lock);
}

void line(const Vector& start, const Vector& end, const Style& style)
{
    if (!validPoint("line", start) || !validPoint("line", end))
        return;
    Batch& b = scratch();
    b.line(start, end);
    commit("line", b, style);
}

void lines(std::span<const Vector> points, bool closed, const Style& style)
{
    if (points.size() < 2)
        return;
    // Past the cap only the newest part would stay, so only that part is built.
    const size_t keep = limits().maxPrimitives + 1;
    const size_t skipped = points.size() > keep ? points.size() - keep : 0;
    if (skipped)
    {
        points = points.last(keep);
        closed = false;
    }
    for (const Vector& p : points)
        if (!validPoint("lines", p))
            return;
    Batch& b = scratch();
    b.skipped = skipped;
    b.segs.reserve(points.size() * 2 + 2);
    for (size_t i = 1; i < points.size(); ++i)
        b.line(points[i - 1], points[i]);
    if (closed && points.size() > 2)
        b.line(points.back(), points.front());
    commit("lines", b, style);
}

void segments(std::span<const Vector> points, const Style& style)
{
    if (points.size() % 2)
        warnLimited("segments", "segments: an odd number of points; the last is ignored");
    const size_t pairs = points.size() / 2, keepPairs = limits().maxPrimitives;
    const size_t skipped = pairs > keepPairs ? pairs - keepPairs : 0;
    if (skipped)
        points = points.subspan(skipped * 2, keepPairs * 2);
    for (const Vector& p : points)
        if (!validPoint("segments", p))
            return;
    Batch& b = scratch();
    b.skipped = skipped;
    b.segs.reserve(points.size());
    for (size_t i = 0; i + 1 < points.size(); i += 2)
        b.line(points[i], points[i + 1]);
    commit("segments", b, style);
}

void arrow(const Vector& start, const Vector& end, double arrowSize, const Style& style)
{
    if (!validPoint("arrow", start) || !validPoint("arrow", end) || !validSize("arrow", arrowSize))
        return;
    Batch& b = scratch();
    b.line(start, end);
    const Vector d = end - start;
    const double len = d.size();
    if (len > 1.0e-9 && arrowSize != 0.0)
    {
        const Vector dir = d / len;
        const double size = std::abs(arrowSize);
        Vector u, v;
        basis(dir, u, v);
        const Vector back = end - dir * size;
        for (const Vector& side : {u, -u, v, -v})
            b.line(end, back + side * (size * 0.5));
    }
    commit("arrow", b, style);
}

namespace
{
std::array<Vector, 8> boxCorners(const Vector& center, const Vector& extent, const math::Quat& rotation)
{
    const math::Quat q = rotation.getNormalized();
    std::array<Vector, 8> c;
    for (int i = 0; i < 8; ++i)
        c[size_t(i)] = center + q.rotateVector(Vector(i & 1 ? extent.x : -extent.x, i & 2 ? extent.y : -extent.y, i & 4 ? extent.z : -extent.z));
    return c;
}
} // namespace

void box(const Vector& center, const Vector& extent, const math::Quat& rotation, const Style& style)
{
    if (!validPoint("box", center) || !validSizes("box", extent) || !validRotation("box", rotation))
        return;
    const auto c = boxCorners(center, extent, rotation);
    Batch& b = scratch();
    for (int i = 0; i < 8; ++i)
        for (int bit : {1, 2, 4})
            if (!(i & bit))
                b.line(c[size_t(i)], c[size_t(i | bit)]);
    commit("box", b, style);
}

void solidBox(const Vector& center, const Vector& extent, const math::Quat& rotation, const Style& style)
{
    if (!validPoint("solidBox", center) || !validSizes("solidBox", extent) || !validRotation("solidBox", rotation))
        return;
    const auto c = boxCorners(center, extent, rotation);
    static constexpr int kFaces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
    Batch& b = scratch();
    for (const auto& f : kFaces)
    {
        b.tri(c[size_t(f[0])], c[size_t(f[1])], c[size_t(f[2])]);
        b.tri(c[size_t(f[0])], c[size_t(f[2])], c[size_t(f[3])]);
    }
    commit("solidBox", b, style);
}

void sphere(const Vector& center, double radius, int segments, const Style& style)
{
    if (!validPoint("sphere", center) || !validSize("sphere", radius))
        return;
    const int n = clampSegments("sphere", segments, 4);
    const int lat = std::max(2, n / 2);
    const double r = std::abs(radius);
    const Vector up = math::axisBasis().up;
    Vector u, v;
    basis(up, u, v);
    Batch& b = scratch();
    for (int j = 1; j < lat; ++j)
    {
        const double phi = math::kPi * double(j) / double(lat);
        b.ring(center + up * (r * std::cos(phi)), u, v, r * std::sin(phi), n);
    }
    for (int i = 0; i < n; ++i)
    {
        const double a = 2.0 * math::kPi * double(i) / double(n);
        b.arc(center, up, u * std::cos(a) + v * std::sin(a), r, lat);
    }
    commit("sphere", b, style);
}

void capsule(const Vector& center, double halfHeight, double radius, const math::Quat& rotation, const Style& style)
{
    if (!validPoint("capsule", center) || !validSize("capsule", halfHeight) || !validSize("capsule", radius) ||
        !validRotation("capsule", rotation))
        return;
    const math::Quat q = rotation.getNormalized();
    const Vector up = q.rotateVector(math::axisBasis().up);
    const Vector u = q.rotateVector(math::axisBasis().forward);
    const Vector v = q.rotateVector(math::axisBasis().right);
    const double r = std::abs(radius);
    const double half = std::max(std::abs(halfHeight) - r, 0.0);
    const Vector top = center + up * half, bottom = center - up * half;
    constexpr int n = 16;
    Batch& b = scratch();
    b.ring(top, u, v, r, n);
    b.ring(bottom, u, v, r, n);
    for (const Vector& side : {u, -u, v, -v})
        b.line(top + side * r, bottom + side * r);
    b.arc(top, u, up, r, n / 2);
    b.arc(top, v, up, r, n / 2);
    b.arc(bottom, u, -up, r, n / 2);
    b.arc(bottom, v, -up, r, n / 2);
    commit("capsule", b, style);
}

void cylinder(const Vector& start, const Vector& end, double radius, int segments, const Style& style)
{
    if (!validPoint("cylinder", start) || !validPoint("cylinder", end) || !validSize("cylinder", radius))
        return;
    const int n = clampSegments("cylinder", segments, 4);
    const Vector axis = unitOr(end - start, math::axisBasis().up);
    Vector u, v;
    basis(axis, u, v);
    const double r = std::abs(radius);
    Batch& b = scratch();
    b.ring(start, u, v, r, n);
    b.ring(end, u, v, r, n);
    for (int i = 0; i < n; ++i)
    {
        const double a = 2.0 * math::kPi * double(i) / double(n);
        const Vector side = (u * std::cos(a) + v * std::sin(a)) * r;
        b.line(start + side, end + side);
    }
    commit("cylinder", b, style);
}

void circle(const Vector& center, double radius, int segments, const Vector& normal, const Style& style)
{
    if (!validPoint("circle", center) || !validSize("circle", radius) || !validDirection("circle", normal))
        return;
    const int n = clampSegments("circle", segments);
    Vector u, v;
    basis(unitOr(normal, math::axisBasis().up), u, v);
    Batch& b = scratch();
    b.ring(center, u, v, std::abs(radius), n);
    commit("circle", b, style);
}

void cone(const Vector& origin, const Vector& direction, double length, double angleWidth, double angleHeight, int sides, const Style& style)
{
    if (!finite(direction) || !std::isfinite(angleWidth) || !std::isfinite(angleHeight))
    {
        reject("cone", "the direction or an angle is NaN or infinite");
        return;
    }
    if (!validPoint("cone", origin) || !validSize("cone", length))
        return;
    const int n = clampSegments("cone", sides);
    const Vector axis = unitOr(direction, math::axisBasis().forward);
    Vector u, v;
    basis(axis, u, v);
    const double tw = std::tan(math::degreesToRadians(std::clamp(std::abs(angleWidth), 0.0, 89.0)));
    const double th = std::tan(math::degreesToRadians(std::clamp(std::abs(angleHeight), 0.0, 89.0)));
    const double len = std::abs(length);
    Batch& b = scratch();
    Vector first, prev;
    for (int i = 0; i < n; ++i)
    {
        const double a = 2.0 * math::kPi * double(i) / double(n);
        const Vector p = origin + (axis + u * (tw * std::cos(a)) + v * (th * std::sin(a))).getSafeNormal() * len;
        b.line(origin, p);
        if (i)
            b.line(prev, p);
        else
            first = p;
        prev = p;
    }
    b.line(prev, first);
    commit("cone", b, style);
}

void coordinateSystem(const Vector& origin, const math::Quat& rotation, double scale, const Style& style)
{
    if (!validPoint("coordinateSystem", origin) || !validSize("coordinateSystem", scale) || !validRotation("coordinateSystem", rotation))
        return;
    const math::Quat q = rotation.getNormalized();
    Batch& b = scratch();
    b.line(origin, origin + q.getAxisX() * scale);
    b.line(origin, origin + q.getAxisY() * scale);
    b.line(origin, origin + q.getAxisZ() * scale);
    b.segColors = {{1.0f, 0.05f, 0.05f, 1.0f}, {0.05f, 1.0f, 0.05f, 1.0f}, {0.1f, 0.25f, 1.0f, 1.0f}};
    commit("coordinateSystem", b, style);
}

void frustum(const math::Transform& transform, double fovDegrees, double aspect, double nearDist, double farDist, const Style& style)
{
    const char* what = "frustum";
    if (!validPoint(what, transform.translation) || !validRotation(what, transform.rotation) || !validSizes(what, transform.scale3d) ||
        !validSize(what, nearDist) || !validSize(what, farDist))
        return;
    if (!std::isfinite(fovDegrees) || !std::isfinite(aspect) || aspect <= 0.0)
    {
        reject(what, "the field of view or aspect is not a positive number");
        return;
    }
    const double fov = std::clamp(fovDegrees, 0.1, 179.0);
    const double n = std::abs(nearDist), f = std::max(std::abs(farDist), n);
    const math::AxisBasis& ax = math::axisBasis();
    const double t = std::tan(math::degreesToRadians(fov) * 0.5);
    auto corner = [&](double d, int i) {
        const double x = (i & 1 ? 1.0 : -1.0) * t * d;
        const double y = (i & 2 ? 1.0 : -1.0) * t * d / aspect;
        return transform.transformPosition(Vector(ax.forward) * d + Vector(ax.right) * x + Vector(ax.up) * y);
    };
    Batch& b = scratch();
    static constexpr int kLoop[4] = {0, 1, 3, 2};
    for (int k = 0; k < 4; ++k)
    {
        const int i = kLoop[k], j = kLoop[(k + 1) % 4];
        b.line(corner(n, i), corner(n, j));
        b.line(corner(f, i), corner(f, j));
        b.line(corner(n, i), corner(f, i));
    }
    commit(what, b, style);
}

void solidPlane(const Vector& point, const Vector& normal, double extent, const Style& style)
{
    if (!validPoint("solidPlane", point) || !validDirection("solidPlane", normal) || !validSize("solidPlane", extent))
        return;
    Vector u, v;
    basis(unitOr(normal, math::axisBasis().up), u, v);
    const double e = std::abs(extent);
    const Vector a = point + (u + v) * e, b2 = point + (v - u) * e, c = point - (u + v) * e, d = point + (u - v) * e;
    Batch& b = scratch();
    b.tri(a, b2, c);
    b.tri(a, c, d);
    commit("solidPlane", b, style);
}

void text(const Vector& position, std::string_view str, const Style& style)
{
    if (!validPoint("text", position) || str.empty())
        return;
    std::string t(str.substr(0, kMaxTextLength));
    if (str.size() > kMaxTextLength)
    {
        // Back off to a whole UTF-8 sequence.
        while (!t.empty() && (uint8_t(t.back()) & 0xC0) == 0x80)
            t.pop_back();
        if (!t.empty() && (uint8_t(t.back()) & 0x80))
            t.pop_back();
        warnLimited("textLength", std::format("text: {} characters cut to {}", str.size(), kMaxTextLength));
    }
    const Limits lim = limits();
    const double time = now();
    Resolved r;
    if (!resolve("text", style, time, lim, r))
        return;
    Store& s = store();
    std::unique_lock lock(s.mutex);
    if (!channelUsable(s, r.meta.channel, "text"))
        return;
    Txt e{glm::vec3(position.toGlm()), r.color, std::move(t), r.meta};
    e.m.seq = ++s.seq;
    s.txts.push_back(std::move(e));
    s.pendingFrame += (r.meta.flags & Frame) ? 1 : 0;
    s.nextExpiry = std::min(s.nextExpiry, r.meta.expiry);
    enforceCap(s, lim);
    notify(s, lock);
}

void flush(Channel ch)
{
    Store& s = store();
    std::unique_lock lock(s.mutex);
    if (ch.id() == 0)
        clearAll(s);
    else if (!eraseChannel(s, ch.id()))
        return;
    notify(s, lock);
}

// ---------------------------------------------------------------- limits and stats

Limits limits()
{
    std::lock_guard lock(gLimitsMutex);
    const double t = now();
    if (!gLimitsOverridden && t - gLimitsRead > 1.0)
    {
        gLimitsRead = t;
        const PrefStore& p = PrefStore::get();
        Limits d;
        auto count = [&](const char* key, size_t fallback, size_t lo, size_t hi) {
            const double v = p.value<double>("general", key, double(fallback));
            return std::isfinite(v) ? std::clamp(size_t(std::max(v, 0.0)), lo, hi) : fallback;
        };
        auto seconds = [&](const char* key, double fallback) {
            const double v = p.value<double>("general", key, fallback);
            return std::isfinite(v) && v >= 0.0 ? v : fallback;
        };
        gLimits.maxPrimitives = count("/debugDraw/maxPrimitives", d.maxPrimitives, 1000, 50'000'000);
        gLimits.maxPoints = count("/debugDraw/maxPoints", d.maxPoints, 10, 10'000'000);
        gLimits.maxTexts = count("/debugDraw/maxTexts", d.maxTexts, 10, 1'000'000);
        gLimits.maxDuration = seconds("/debugDraw/maxDuration", d.maxDuration);
        gLimits.defaultDuration = std::min(seconds("/debugDraw/defaultDuration", d.defaultDuration), gLimits.maxDuration);
    }
    return gLimits;
}

void setLimits(const Limits* l)
{
    std::lock_guard lock(gLimitsMutex);
    gLimitsOverridden = l != nullptr;
    gLimits = l ? *l : Limits{};
    gLimitsRead = -kInf;
}

Stats stats()
{
    Store& s = store();
    std::lock_guard lock(s.mutex);
    return {s.segs.size(), s.tris.size(), s.pts.size(), s.txts.size(), s.channels.size(), s.dropped, gRejected.load()};
}

// ---------------------------------------------------------------- for viewports

double now()
{
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

std::shared_ptr<const Snapshot> snapshot(double time)
{
    LILA_ZONE();
    Store& s = store();
    std::lock_guard lock(s.mutex);
    bool changed = false;
    if (s.pendingFrame)
    {
        // The first view to draw them starts their short grace.
        auto settle = [&](auto& d) {
            for (auto& e : d)
                if (e.m.flags & Frame)
                {
                    e.m.flags &= uint8_t(~Frame);
                    e.m.expiry = std::min(e.m.expiry, time + kFrameGrace);
                }
        };
        settle(s.segs);
        settle(s.tris);
        settle(s.pts);
        settle(s.txts);
        s.pendingFrame = 0;
        s.nextExpiry = std::min(s.nextExpiry, time + kFrameGrace);
    }
    if (time >= s.nextExpiry)
    {
        auto expired = [time](const auto& e) { return e.m.expiry <= time; };
        changed |= eraseIf(s.segs, s.pendingFrame, expired) + eraseIf(s.tris, s.pendingFrame, expired) + eraseIf(s.pts, s.pendingFrame, expired) +
                       eraseIf(s.txts, s.pendingFrame, expired) >
                   0;
        double next = kInf;
        auto scan = [&](const auto& d) {
            for (const auto& e : d)
                next = std::min(next, e.m.expiry);
        };
        scan(s.segs);
        scan(s.tris);
        scan(s.pts);
        scan(s.txts);
        s.nextExpiry = next;
    }
    if (changed)
        ++s.generation;
    if (s.cached && s.cached->generation == s.generation)
        return s.cached;

    auto snap = std::make_shared<Snapshot>();
    snap->generation = s.generation;
    auto shown = [&](uint32_t channel) {
        if (!channel)
            return true;
        auto it = s.channels.find(channel);
        return it != s.channels.end() && it->second.visible;
    };
    for (const Seg& e : s.segs)
    {
        if (!shown(e.m.channel))
            continue;
        const bool top = e.m.flags & OnTop;
        auto it = std::find_if(snap->lines.begin(), snap->lines.end(), [&](const LineBatch& lb) { return lb.width == e.width && lb.onTop == top; });
        if (it == snap->lines.end())
        {
            snap->lines.push_back({e.width, top, {}});
            it = snap->lines.end() - 1;
        }
        it->vertices.push_back({e.a, e.color});
        it->vertices.push_back({e.b, e.color});
    }
    for (const Tri& e : s.tris)
        if (shown(e.m.channel))
            for (const glm::vec3& p : e.p)
                snap->triangles.push_back({p, e.color});
    for (const Pt& e : s.pts)
        if (shown(e.m.channel))
            snap->points.push_back({e.pos, e.color, e.size});
    for (const Txt& e : s.txts)
        if (shown(e.m.channel))
            snap->texts.push_back({e.pos, e.color, e.text});
    s.cached = snap;
    return snap;
}

double nextExpiry()
{
    Store& s = store();
    std::lock_guard lock(s.mutex);
    return s.nextExpiry;
}

uint64_t generation()
{
    Store& s = store();
    std::lock_guard lock(s.mutex);
    return s.generation;
}

void setChangedHook(std::function<void()> hook)
{
    Store& s = store();
    std::lock_guard lock(s.mutex);
    s.hook = hook ? std::make_shared<const std::function<void()>>(std::move(hook)) : nullptr;
}
} // namespace lila::debugDraw
