#include <doctest/doctest.h>

#include "core/Events.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Registry.h"
#include "debug/DebugDraw.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace lila;
namespace dd = lila::debugDraw;
using math::LinearColor;
using math::Vector;

namespace
{
/** A clean store with known limits, put back afterwards. */
struct Fresh
{
    explicit Fresh(dd::Limits l = {})
    {
        dd::setLimits(&l);
        dd::flush();
        dd::setChangedHook({});
    }
    ~Fresh()
    {
        dd::flush();
        dd::setLimits(nullptr);
        dd::setChangedHook({});
    }
};

/** Counts warnings logged while alive that contain needle. */
struct Warnings
{
    explicit Warnings(std::string needle) : mNeedle(std::move(needle))
    {
        mSink = log::addSink([this](const LogEntry& e) {
            if (e.level == LogLevel::Warning && e.text.find(mNeedle) != std::string::npos)
                ++count;
        });
    }
    ~Warnings() { log::removeSink(mSink); }
    std::atomic<int> count{0};

private:
    std::string mNeedle;
    int mSink = 0;
};

size_t lineCount(const dd::Snapshot& s)
{
    size_t n = 0;
    for (const dd::LineBatch& b : s.lines)
        n += b.vertices.size() / 2;
    return n;
}

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
} // namespace

TEST_CASE("debug draw: every draw expires unless persistent in a channel")
{
    Fresh fresh({.defaultDuration = 5.0});
    const double t = dd::now();
    dd::line({0, 0, 0}, {1, 0, 0});
    dd::line({0, 0, 0}, {0, 1, 0}, {.duration = 1.0});
    CHECK(lineCount(*dd::snapshot(t)) == 2);
    CHECK(dd::nextExpiry() <= t + 1.5);
    CHECK(lineCount(*dd::snapshot(t + 2.0)) == 1);
    CHECK(lineCount(*dd::snapshot(t + 6.0)) == 0);
    CHECK(std::isinf(dd::nextExpiry()));

    SUBCASE("duration 0 lasts for the views drawn next, then goes")
    {
        const double t0 = dd::now();
        dd::line({0, 0, 0}, {1, 0, 0}, {.duration = 0.0});
        CHECK(lineCount(*dd::snapshot(t0 + 0.5)) == 1);
        CHECK(lineCount(*dd::snapshot(t0 + 0.52)) == 1); // a second view in the same pass
        CHECK(lineCount(*dd::snapshot(t0 + 0.6)) == 0);
    }
    SUBCASE("duration 0 never drawn still goes")
    {
        const double t0 = dd::now();
        dd::text({0, 0, 0}, "hi", {.duration = 0.0});
        CHECK(dd::stats().texts == 1);
        CHECK(dd::snapshot(t0 + 3.0)->texts.empty());
    }
    SUBCASE("persistent without a channel is refused and expires")
    {
        Warnings w("persistent draws need a channel");
        const double t0 = dd::now();
        dd::line({0, 0, 0}, {1, 0, 0}, {.persistent = true});
        CHECK(w.count == 1);
        CHECK(lineCount(*dd::snapshot(t0 + 6.0)) == 0);
    }
    SUBCASE("persistent in a channel stays until cleared")
    {
        dd::Channel ch = dd::channel("persist", "test");
        dd::Channel other = dd::channel("other", "test");
        const double t0 = dd::now();
        dd::box({0, 0, 0}, {10, 10, 10}, {}, {.persistent = true, .channel = ch});
        dd::line({0, 0, 0}, {1, 0, 0}, {.persistent = true, .channel = other});
        CHECK(lineCount(*dd::snapshot(t0 + 1.0e7)) == 13);
        CHECK(ch.primitiveCount() == 12);
        ch.clear();
        CHECK(ch.valid());
        CHECK(lineCount(*dd::snapshot(t0 + 1.0e7)) == 1);
        dd::flush();
        CHECK(lineCount(*dd::snapshot(t0 + 1.0e7)) == 0);
        ch.remove();
        other.remove();
    }
    SUBCASE("durations past the cap are clamped")
    {
        dd::Limits l{.maxDuration = 10.0};
        dd::setLimits(&l);
        Warnings w("clamped");
        const double t0 = dd::now();
        dd::line({0, 0, 0}, {1, 0, 0}, {.duration = 1.0e9});
        CHECK(w.count == 1);
        CHECK(lineCount(*dd::snapshot(t0 + 9.0)) == 1);
        CHECK(lineCount(*dd::snapshot(t0 + 11.0)) == 0);
    }
}

TEST_CASE("debug draw: channels are cleared with the scene and their owner")
{
    Fresh fresh;
    SUBCASE("same name and owner, same channel")
    {
        dd::Channel a = dd::channel("tool", "ownerA");
        CHECK(dd::channel("tool", "ownerA") == a);
        CHECK(dd::channel("tool", "ownerB") != a);
        CHECK(a.name() == "tool");
        CHECK(a.owner() == "ownerA");
        a.remove();
        dd::channel("tool", "ownerB").remove();
    }
    SUBCASE("new and open scene clear every channel")
    {
        dd::Channel ch = dd::channel("scene", "test");
        dd::line({0, 0, 0}, {1, 0, 0}, {.persistent = true, .channel = ch});
        events::send({.type = events::Event::SceneNew});
        CHECK(dd::stats().lines == 0);
        dd::line({0, 0, 0}, {1, 0, 0}, {.persistent = true, .channel = ch});
        events::send({.type = events::Event::SceneOpened});
        CHECK(dd::stats().lines == 0);
        CHECK(ch.valid());
        ch.remove();
    }
    SUBCASE("an extension's channels go when it unloads")
    {
        dd::Channel ch;
        {
            ext::Context ctx("debugDrawTestExtension");
            ch = dd::channel(ctx, "paths");
            CHECK(dd::channel(ctx, "paths") == ch);
            CHECK(ctx.registrationCount() == 1);
            dd::line({0, 0, 0}, {1, 0, 0}, {.persistent = true, .channel = ch});
            CHECK(dd::stats().lines == 1);
            ctx.unloadAll();
        }
        CHECK_FALSE(ch.valid());
        CHECK(dd::stats().lines == 0);
        Warnings w("channel was removed");
        dd::line({0, 0, 0}, {1, 0, 0}, {.persistent = true, .channel = ch});
        CHECK(dd::stats().lines == 0);
        CHECK(w.count == 1);
    }
    SUBCASE("removing an owner's registrations removes its channels (Python extensions)")
    {
        dd::Channel ch = dd::channel("py", "debugDrawPyExtension");
        dd::text({0, 0, 0}, "label", {.persistent = true, .channel = ch});
        CHECK(owners::removeOwned("debugDrawPyExtension") >= 1);
        CHECK_FALSE(ch.valid());
        CHECK(dd::stats().texts == 0);
    }
    SUBCASE("a hidden channel is kept but not drawn")
    {
        dd::Channel ch = dd::channel("hidden", "test");
        dd::line({0, 0, 0}, {1, 0, 0}, {.channel = ch});
        dd::line({0, 0, 0}, {2, 0, 0});
        ch.setVisible(false);
        CHECK(lineCount(*dd::snapshot()) == 1);
        ch.setVisible(true);
        CHECK(lineCount(*dd::snapshot()) == 2);
        ch.remove();
    }
}

TEST_CASE("debug draw: caps drop the oldest with one warning")
{
    Fresh fresh({.maxPrimitives = 1000, .maxPoints = 50, .maxTexts = 20});
    Warnings w("over the limit");
    for (int i = 0; i < 1500; ++i)
        dd::line({double(i), 0, 0}, {double(i), 1, 0});
    dd::Stats s = dd::stats();
    CHECK(s.lines == 1000);
    CHECK(s.dropped == 500);
    CHECK(w.count == 1);
    std::shared_ptr<const dd::Snapshot> snap = dd::snapshot();
    REQUIRE(snap->lines.size() == 1);
    CHECK(snap->lines[0].vertices.front().pos.x == doctest::Approx(500.0));
    CHECK(snap->lines[0].vertices.back().pos.x == doctest::Approx(1499.0));

    SUBCASE("one call bigger than the cap keeps its newest part")
    {
        dd::flush();
        std::vector<Vector> pts;
        for (int i = 0; i < 5000; ++i)
            pts.emplace_back(double(i), 0.0, 0.0);
        dd::lines(pts);
        CHECK(dd::stats().lines == 1000);
        CHECK(w.count == 2);
        CHECK(dd::snapshot()->lines[0].vertices.back().pos.x == doctest::Approx(4999.0));
    }
    SUBCASE("points and texts have caps of their own")
    {
        for (int i = 0; i < 100; ++i)
        {
            dd::point({double(i), 0, 0});
            dd::text({double(i), 0, 0}, "t");
        }
        CHECK(dd::stats().points == 50);
        CHECK(dd::stats().texts == 20);
        CHECK(dd::stats().lines == 1000);
    }
    SUBCASE("a full flush rearms the warning")
    {
        dd::flush();
        CHECK(dd::stats().dropped == 0);
        for (int i = 0; i < 1001; ++i)
            dd::line({0, 0, 0}, {1, 0, 0});
        CHECK(w.count == 2);
    }
}

TEST_CASE("debug draw: invalid input never draws")
{
    Fresh fresh;
    const size_t rejected = dd::stats().rejected;
    dd::line({kNaN, 0, 0}, {1, 0, 0});
    dd::line({0, 0, 0}, {kInf, 0, 0});
    dd::line({1.0e12, 0, 0}, {0, 0, 0});
    dd::sphere({0, 0, 0}, kNaN);
    dd::sphere({0, 0, 0}, 1.0e12);
    dd::box({0, 0, 0}, {1, 1, 1}, math::Quat(0, 0, 0, 0));
    dd::box({0, 0, 0}, {1, kNaN, 1});
    dd::circle({0, 0, 0}, 10.0, 32, {kNaN, 0, 0});
    dd::cone({0, 0, 0}, {1, 0, 0}, 10.0, kNaN, 10.0);
    dd::frustum(math::Transform(), 90.0, 0.0, 1.0, 10.0);
    dd::text({kNaN, 0, 0}, "nope");
    dd::point({0, kInf, 0});
    std::vector<Vector> pts{{0, 0, 0}, {1, 0, 0}, {kNaN, 0, 0}};
    dd::lines(pts);
    dd::Stats s = dd::stats();
    CHECK(s.lines == 0);
    CHECK(s.texts == 0);
    CHECK(s.points == 0);
    CHECK(s.rejected - rejected == 13);

    SUBCASE("segment counts, thickness and colors are clamped")
    {
        dd::sphere({0, 0, 0}, 10.0, 1'000'000);
        CHECK(dd::stats().lines == 127 * 256 + 256 * 128);
        dd::flush();
        dd::circle({0, 0, 0}, 10.0, -5);
        CHECK(dd::stats().lines == size_t(dd::kMinSegments));
        dd::flush();
        dd::line({0, 0, 0}, {1, 0, 0}, {.color = LinearColor(float(kNaN), 0, 0), .thickness = 1000.0f});
        std::shared_ptr<const dd::Snapshot> snap = dd::snapshot();
        REQUIRE(snap->lines.size() == 1);
        CHECK(snap->lines[0].width == dd::kMaxThickness);
        CHECK(snap->lines[0].vertices[0].color == glm::vec4(1.0f));
    }
    SUBCASE("long text is cut on a character boundary")
    {
        std::string longText;
        while (longText.size() < 3000)
            longText += "\xC3\xA9"; // two-byte UTF-8
        dd::text({0, 0, 0}, longText);
        std::shared_ptr<const dd::Snapshot> snap = dd::snapshot();
        REQUIRE(snap->texts.size() == 1);
        const std::string& t = snap->texts[0].text;
        CHECK(t.size() <= dd::kMaxTextLength);
        CHECK(t.size() % 2 == 0);
    }
}

TEST_CASE("debug draw: shapes")
{
    Fresh fresh;
    dd::box({0, 0, 0}, {1, 2, 3}, math::Quat(math::Rotator(0, 45, 0)));
    CHECK(dd::stats().lines == 12);
    dd::flush();
    dd::solidBox({0, 0, 0}, {1, 1, 1});
    CHECK(dd::stats().triangles == 12);
    CHECK(dd::stats().lines == 0);
    dd::flush();
    dd::coordinateSystem({0, 0, 0});
    std::shared_ptr<const dd::Snapshot> snap = dd::snapshot();
    REQUIRE(lineCount(*snap) == 3);
    CHECK(snap->lines[0].vertices[0].color.r == doctest::Approx(1.0f));
    CHECK(snap->lines[0].vertices[2].color.g == doctest::Approx(1.0f));
    CHECK(snap->lines[0].vertices[4].color.b == doctest::Approx(1.0f));
    dd::flush();
    dd::arrow({0, 0, 0}, {0, 0, 0}, 10.0); // zero length: a line, no head
    dd::arrow({0, 0, 0}, {100, 0, 0}, 10.0);
    CHECK(dd::stats().lines == 1 + 5);
    dd::flush();
    dd::frustum(math::Transform(), 90.0, 1.5, 10.0, 100.0);
    dd::capsule({0, 0, 0}, 50.0, 10.0);
    dd::cylinder({0, 0, 0}, {0, 100, 0}, 10.0, 8);
    dd::cone({0, 0, 0}, {1, 0, 0}, 100.0, 20.0, 10.0, 8);
    dd::solidPlane({0, 0, 0}, {0, 1, 0}, 50.0);
    CHECK(dd::stats().lines == 12 + (16 * 2 + 4 + 8 * 4) + (8 * 3) + 8 * 2);
    CHECK(dd::stats().triangles == 2);

    SUBCASE("lines batch by width and depth mode")
    {
        dd::flush();
        dd::line({0, 0, 0}, {1, 0, 0}, {.thickness = 2.0f});
        dd::line({0, 0, 0}, {1, 0, 0}, {.thickness = 2.1f});
        dd::line({0, 0, 0}, {1, 0, 0}, {.thickness = 2.0f, .onTop = true});
        dd::line({0, 0, 0}, {1, 0, 0});
        snap = dd::snapshot();
        CHECK(snap->lines.size() == 3);
        CHECK(lineCount(*snap) == 4);
    }
}

TEST_CASE("debug draw: any thread may draw while views read")
{
    Fresh fresh({.defaultDuration = 600.0});
    std::atomic<int> changes{0};
    dd::setChangedHook([&changes] { ++changes; });
    constexpr int kThreads = 8, kEach = 5000;
    std::atomic<bool> done{false};
    std::thread reader([&done] {
        while (!done)
            dd::snapshot();
    });
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t)
        writers.emplace_back([t] {
            dd::Channel ch = dd::channel("thread" + std::to_string(t % 2), "threads");
            for (int i = 0; i < kEach; ++i)
            {
                dd::line({double(i), double(t), 0}, {double(i), double(t) + 1.0, 0}, {.channel = i % 2 ? ch : dd::Channel()});
                if (i % 100 == 0)
                    dd::text({0, 0, 0}, "x", {.duration = 0.0});
            }
        });
    for (std::thread& w : writers)
        w.join();
    done = true;
    reader.join();
    CHECK(dd::stats().lines == size_t(kThreads * kEach));
    CHECK(changes >= kThreads * kEach);
    CHECK(lineCount(*dd::snapshot()) == size_t(kThreads * kEach));
    dd::channel("thread0", "threads").remove();
    dd::channel("thread1", "threads").remove();
}
