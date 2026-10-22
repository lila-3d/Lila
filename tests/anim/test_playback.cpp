#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "scene/Anim.h"
#include "scene/PlaybackClock.h"

using namespace lila;
using json = nlohmann::json;
using namespace lila::scene::anim;

TEST_CASE("playback clock: real time drops frames to keep pace")
{
    PlaybackClock c;
    c.reset(10, 0.0);
    CHECK_FALSE(c.advance(PlaybackSpeed::RealTime, 24.0, 1, 0.01));
    CHECK(c.advance(PlaybackSpeed::RealTime, 24.0, 1, 1.0 / 24.0));
    CHECK(c.frame == 11);
    // A long stall skips straight to the frame that is due.
    CHECK(c.advance(PlaybackSpeed::RealTime, 24.0, 1, 0.5));
    CHECK(c.frame == 22);
}

TEST_CASE("playback clock: half and twice scale real time")
{
    PlaybackClock c;
    c.reset(0, 0.0);
    c.advance(PlaybackSpeed::Half, 30.0, 1, 1.0);
    CHECK(c.frame == 15);
    c.reset(0, 0.0);
    c.advance(PlaybackSpeed::Twice, 30.0, 1, 1.0);
    CHECK(c.frame == 60);
    c.reset(100, 0.0);
    c.advance(PlaybackSpeed::RealTime, 30.0, -1, 1.0);
    CHECK(c.frame == 70);
}

TEST_CASE("playback clock: every frame steps once per tick regardless of time")
{
    PlaybackClock c;
    c.reset(5, 0.0);
    for (int i = 0; i < 3; ++i)
        CHECK(c.advance(PlaybackSpeed::EveryFrame, 30.0, 1, 0.0));
    CHECK(c.frame == 8);
    CHECK(c.advance(PlaybackSpeed::EveryFrame, 30.0, -1, 100.0));
    CHECK(c.frame == 7);
}

TEST_CASE("playback clock: every frame max real time never skips or runs ahead")
{
    const double fps = 10.0;
    PlaybackClock c;
    c.reset(0, 0.0);
    CHECK_FALSE(c.advance(PlaybackSpeed::EveryFrameMaxRealTime, fps, 1, 0.05));
    CHECK(c.advance(PlaybackSpeed::EveryFrameMaxRealTime, fps, 1, 0.1));
    CHECK(c.frame == 1);
    CHECK_FALSE(c.advance(PlaybackSpeed::EveryFrameMaxRealTime, fps, 1, 0.15));
    // A one second stall still shows only the next frame, then paces from there instead of bursting.
    CHECK(c.advance(PlaybackSpeed::EveryFrameMaxRealTime, fps, 1, 1.2));
    CHECK(c.frame == 2);
    CHECK_FALSE(c.advance(PlaybackSpeed::EveryFrameMaxRealTime, fps, 1, 1.21));
    CHECK(c.advance(PlaybackSpeed::EveryFrameMaxRealTime, fps, 1, 1.3));
    CHECK(c.frame == 3);
}

TEST_CASE("playback clock: a scrub during playback continues from the scrubbed frame")
{
    PlaybackClock c;
    c.reset(0, 0.0);
    c.advance(PlaybackSpeed::EveryFrame, 30.0, 1, 0.0);
    c.resync(40, 1.0);
    CHECK(c.advance(PlaybackSpeed::EveryFrame, 30.0, 1, 1.0));
    CHECK(c.frame == 41);

    c.reset(0, 0.0);
    c.advance(PlaybackSpeed::EveryFrameMaxRealTime, 10.0, 1, 0.1);
    c.resync(50, 2.0);
    CHECK_FALSE(c.advance(PlaybackSpeed::EveryFrameMaxRealTime, 10.0, 1, 2.05));
    CHECK(c.advance(PlaybackSpeed::EveryFrameMaxRealTime, 10.0, 1, 2.1));
    CHECK(c.frame == 51);

    c.reset(0, 0.0);
    c.resync(20, 1.0);
    c.advance(PlaybackSpeed::RealTime, 30.0, 1, 2.0);
    CHECK(c.frame == 50);

    // The frame playback itself showed is not a scrub.
    c.reset(3, 0.0);
    c.resync(3, 5.0);
    CHECK(c.originSeconds == 0.0);
}

TEST_CASE("playback speed names round-trip")
{
    for (PlaybackSpeed s : kPlaybackSpeeds)
        CHECK(speedFromName(speedName(s)) == s);
    CHECK_FALSE(speedFromName("bogus"));
}

TEST_CASE("playbackOptions sets the playback speed")
{
    PlaybackSpeed before = playback().speed;
    json r = cmds::execute("playbackOptions", {{"playbackSpeed", "everyFrame"}});
    CHECK(r["playbackSpeed"] == "everyFrame");
    CHECK(r["everyFrame"] == true);
    CHECK(playback().speed == PlaybackSpeed::EveryFrame);
    cmds::execute("playbackOptions", {{"everyFrame", false}});
    CHECK(playback().speed == PlaybackSpeed::RealTime);
    CHECK_THROWS_AS(cmds::execute("playbackOptions", {{"playbackSpeed", "warp"}}), CommandError);
    // Scene data does not carry the speed.
    playback().fromJson({{"playbackSpeed", "twice"}});
    CHECK(playback().speed == PlaybackSpeed::RealTime);
    cmds::execute("playbackOptions", {{"playbackSpeed", std::string(speedName(before))}});
}

TEST_CASE("playback clock: custom speed scales real time by the multiplier")
{
    PlaybackClock c;
    c.reset(0, 0.0);
    c.advance(PlaybackSpeed::Custom, 24.0, 1, 1.0, 0.25);
    CHECK(c.frame == 6);
    c.reset(0, 0.0);
    CHECK_FALSE(c.advance(PlaybackSpeed::Custom, 24.0, 1, 1.0 / 24.0, 0.5));
    CHECK(c.advance(PlaybackSpeed::Custom, 24.0, 1, 2.0 / 24.0, 0.5));
    CHECK(c.frame == 1);
    c.reset(100, 0.0);
    c.advance(PlaybackSpeed::Custom, 30.0, -1, 2.0, 1.5);
    CHECK(c.frame == 10);
    // The multiplier only applies to the custom speed.
    c.reset(0, 0.0);
    c.advance(PlaybackSpeed::RealTime, 30.0, 1, 1.0, 3.0);
    CHECK(c.frame == 30);
}

TEST_CASE("playbackOptions sets a custom speed multiplier")
{
    Playback before = playback();
    json r = cmds::execute("playbackOptions", {{"speedMultiplier", 0.25}});
    CHECK(r["playbackSpeed"] == "custom");
    CHECK(r["speedMultiplier"] == doctest::Approx(0.25));
    CHECK(playback().speed == PlaybackSpeed::Custom);
    CHECK(playback().customSpeed == doctest::Approx(0.25));
    // An explicit speed wins; the multiplier is kept for later.
    cmds::execute("playbackOptions", {{"playbackSpeed", "realTime"}, {"speedMultiplier", 1.5}});
    CHECK(playback().speed == PlaybackSpeed::RealTime);
    CHECK(playback().customSpeed == doctest::Approx(1.5));
    cmds::execute("playbackOptions", {{"playbackSpeed", "custom"}});
    CHECK(playback().customSpeed == doctest::Approx(1.5));
    CHECK_THROWS_AS(cmds::execute("playbackOptions", {{"speedMultiplier", 0.0}}), CommandError);
    CHECK_THROWS_AS(cmds::execute("playbackOptions", {{"speedMultiplier", -2.0}}), CommandError);
    CHECK(playback().customSpeed == doctest::Approx(1.5));
    playback().fromJson({{"speedMultiplier", 4.0}});
    CHECK(playback().customSpeed == doctest::Approx(1.5));
    cmds::execute("playbackOptions", {{"playbackSpeed", std::string(speedName(before.speed))}, {"speedMultiplier", before.customSpeed}});
}
