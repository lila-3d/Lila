#include <doctest/doctest.h>

#include "TestClips.h"

#include "media/SoundCues.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace
{
using namespace lila;
using media::EventType;
using media::SimEvent;

json cueRun(const char* cmd, const json& call = json::object())
{
    return ops::execute(cmd, call);
}

media::Cue cue()
{
    media::Cue c;
    c.name = "hits";
    c.types = 1 << int(EventType::Contact);
    c.minStrength = 100.0;
    c.fullStrength = 300.0;
    c.sounds = {"a.wav", "b.wav", "c.wav"};
    c.volume = 1.0;
    c.minVolume = 0.2;
    c.minInterval = 0.1;
    return c;
}
} // namespace

TEST_CASE("cues: filter by event type, strength and object")
{
    media::Cue c = cue();
    CHECK(media::matches(c, {EventType::Contact, 1.0, "|wall", 3, 150.0}));
    CHECK_FALSE(media::matches(c, {EventType::Break, 1.0, "|wall", 3, 150.0}));
    CHECK_FALSE(media::matches(c, {EventType::Contact, 1.0, "|wall", 3, 99.0}));
    c.objects = {"wall"};
    CHECK(media::matches(c, {EventType::Contact, 1.0, "|grp|wall", 3, 150.0}));
    CHECK_FALSE(media::matches(c, {EventType::Contact, 1.0, "|floor", 3, 150.0}));
    c.types = 0b11;
    CHECK(media::matches(c, {EventType::Break, 1.0, "|wall", -1, 150.0}));
}

TEST_CASE("cues: volume follows strength, repeats thin out, the pick is random but repeatable")
{
    media::Cue c = cue();
    std::vector<SimEvent> events = {
        {EventType::Contact, 1.00, "|wall", 0, 100.0}, {EventType::Contact, 1.05, "|wall", 0, 300.0}, // same piece, too soon
        {EventType::Contact, 1.05, "|wall", 1, 200.0}, {EventType::Contact, 1.20, "|wall", 0, 900.0},
        {EventType::Contact, 1.30, "|wall", 2, 50.0}, // too weak
    };
    std::vector<media::Trigger> t = media::triggers(c, events);
    REQUIRE(t.size() == 3);
    CHECK(t[0].gain == doctest::Approx(0.2));
    CHECK(t[1].gain == doctest::Approx(0.6));
    CHECK(t[2].gain == doctest::Approx(1.0));
    CHECK(t[1].event.piece == 1);
    CHECK(t[2].time == doctest::Approx(1.2));

    std::vector<media::Trigger> again = media::triggers(c, events);
    for (size_t i = 0; i < t.size(); ++i)
        CHECK(t[i].sound == again[i].sound);

    // Many events spread over the sounds.
    std::vector<SimEvent> many;
    for (int i = 0; i < 60; ++i)
        many.push_back({EventType::Contact, i * 0.2, "|wall", i % 7, 250.0});
    std::set<std::string> used;
    for (const media::Trigger& tr : media::triggers(c, many))
        used.insert(tr.sound);
    CHECK(used.size() == 3);
}

TEST_CASE("cues: soundCue nodes read events from the registered sources")
{
    cueRun("newScene");
    cueRun("spaceLocator", {{"name", "crate"}});
    media::eventSources().put("test", {"test", [](scene::Graph&, double t0, double t1) {
                                           std::vector<SimEvent> out;
                                           for (double t : {0.5, 1.0, 1.5})
                                               if (t > t0 && t <= t1)
                                                   out.push_back({EventType::Break, t, "|crate", 1, 40.0});
                                           out.push_back({EventType::Contact, 1.0, "|other", 0, 500.0});
                                           if (!(1.0 > t0 && 1.0 <= t1))
                                               out.pop_back();
                                           return out;
                                       }});
    CHECK(cueRun("soundCue", {{"name", "cracks"}, {"eventType", "break"}, {"minStrength", 10.0}, {"fullStrength", 40.0}, {"targets", "crate"},
                           {"sounds", {"sfx/crack1.wav", "sfx/crack2.wav"}}}) == "cracks");
    json fired = cueRun("soundCueTriggers", {{"start", 0.0}, {"end", 60.0}});
    REQUIRE(fired.size() == 3);
    CHECK(fired[0]["event"] == "break");
    CHECK(fired[0]["gain"] == doctest::Approx(1.0));
    CHECK(fired[1]["time"] == doctest::Approx(1.0));

    // A set names its members.
    cueRun("createSet", {{"args", {"crate"}}, {"name", "debris"}});
    cueRun("soundCue", {{"args", {"cracks"}}, {"targets", "debris"}});
    CHECK(cueRun("soundCueTriggers", {{"start", 0.0}, {"end", 60.0}}).size() == 3);

    cueRun("soundCue", {{"args", {"cracks"}}, {"mute", true}});
    CHECK(cueRun("soundCueTriggers", {{"start", 0.0}, {"end", 60.0}}).empty());
    cueRun("soundCue", {{"args", {"cracks"}}, {"mute", false}});

    const auto csv = mediatest::scratch() / "cues.csv";
    json res = cueRun("exportSoundCues", {{"path", csv.string()}, {"profile", "generic"}});
    CHECK(res["sounds"] == 3);
    std::ifstream in(csv);
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().rfind("---,Frame,Time,X,Y,Z,Sound,File,Volume,Cue,Event,Object,Piece,Strength\n", 0) == 0);
    CHECK(text.str().find(",crack") != std::string::npos);
    CHECK(text.str().find(",Break,crate,1,") != std::string::npos);
    media::eventSources().remove("test");
}
