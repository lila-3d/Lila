// SPDX-License-Identifier: Apache-2.0
// Attributes (strand channels): which params write and which read them (ParamSpec::channel), and
// graphChannels: what reaches each node, what it writes, what it overwrites, whether its reads reach.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/attributes.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/operators.h"

namespace groomlab {
namespace {

using json = nlohmann::json;

// Scalp -> Scatter -> Interpolate (+ Guides) -> `chain`, each fed the one before it (its "strands"
// input), the last the output. Each entry: {name, type, params}.
json chainGraph(const std::vector<json>& chain, const json& interpolateParams = json::object()) {
    json g = {{"version", 1},
              {"nodes", json::array({{{"name", "scalp"}, {"type", "Scalp"}},
                                     {{"name", "guides"}, {"type", "Guides"}},
                                     {{"name", "scatter"}, {"type", "Scatter"}},
                                     {{"name", "interpolate"}, {"type", "Interpolate"}, {"params", interpolateParams}}})},
              {"connections", json::array({{{"from", "scalp"}, {"to", "scatter"}, {"input", "scalp"}},
                                           {{"from", "scatter"}, {"to", "interpolate"}, {"input", "strands"}},
                                           {{"from", "guides"}, {"to", "interpolate"}, {"input", "guides"}},
                                           {{"from", "scalp"}, {"to", "interpolate"}, {"input", "scalp"}}})}};
    std::string last = "interpolate";
    for (const json& n : chain) {
        g["nodes"].push_back(n);
        const std::string name = n.at("name").get<std::string>();
        g["connections"].push_back({{"from", last}, {"to", name}, {"input", "strands"}});
        last = name;
    }
    g["output"] = last;
    return g;
}

json node(const std::string& name, const std::string& type, const json& params = json::object()) {
    return {{"name", name}, {"type", type}, {"params", params}};
}

const ChannelSource* incoming(const NodeChannels& n, const std::string& channel) {
    for (const ChannelSource& c : n.incoming) {
        if (c.name == channel) return &c;
    }
    return nullptr;
}

// The node that wrote `channel` last of those reaching the node ("" when none does).
std::string sourceOf(const NodeChannels& n, const std::string& channel) {
    const ChannelSource* c = incoming(n, channel);
    return c ? c->node : std::string();
}

const ChannelRead* read(const NodeChannels& n, const std::string& param) {
    for (const ChannelRead& r : n.reads) {
        if (r.param == param) return &r;
    }
    return nullptr;
}

std::vector<std::string> writtenNames(const NodeChannels& n) {
    std::vector<std::string> out;
    for (const ChannelWrite& w : n.writes) out.push_back(w.name);
    return out;
}

TEST(AttributeFlow, SpecsSayWhichParamsWriteAndWhichRead) {
    const json specs = json::parse(operatorSpecsJson());
    auto channelOf = [&](const std::string& type, const std::string& param) {
        for (const json& op : specs) {
            if (op.at("type") != type) continue;
            for (const json& p : op.at("params")) {
                if (p.at("name") == param) return p.at("channel").get<std::string>();
            }
        }
        return std::string("missing");
    };
    EXPECT_EQ(channelOf("Clump", "clumpChannel"), "writes");
    EXPECT_EQ(channelOf("Clump", "regionChannel"), "reads");
    EXPECT_EQ(channelOf("Clump", "strength"), "");
    EXPECT_EQ(channelOf("Curl", "clumpChannel"), "reads");
    EXPECT_EQ(channelOf("Twist", "clumpChannel"), "reads");
    EXPECT_EQ(channelOf("Cards", "clumpChannel"), "reads");
    EXPECT_EQ(channelOf("HairColor", "clumpChannel"), "reads");
    EXPECT_EQ(channelOf("Wet", "clumpChannel"), "writes");
    EXPECT_EQ(channelOf("Attribute", "name"), "writes");
    EXPECT_EQ(channelOf("Group", "name"), "writes");
    EXPECT_EQ(channelOf("Blend", "mixChannel"), "writes");
    EXPECT_EQ(channelOf("Interpolate", "influenceChannel"), "writes");
    EXPECT_EQ(channelOf("Interpolate", "regionChannel"), "writes");
    EXPECT_EQ(channelOf("Plume", "rankChannel"), "reads");
    for (const json& op : specs) {
        if (op.at("type") == "Flyaway") EXPECT_EQ(op.at("fixedChannels"), json::array({"flyaway"}));
        if (op.at("type") == "Clump") EXPECT_TRUE(op.at("fixedChannels").empty());
        if (op.at("type") == "Wet") {
            const json fixed = op.at("fixedChannels");
            EXPECT_NE(std::find(fixed.begin(), fixed.end(), "wetness"), fixed.end());
        }
    }
}

TEST(AttributeFlow, ChainedClumpsWithTheirOwnNamesEachReachTheNodesAfter) {
    const json g = chainGraph({node("clumpLarge", "Clump", {{"clumpChannel", "lockId"}}),
                               node("clumpSmall", "Clump", {{"clumpChannel", "clumpId"}}),
                               node("curl", "Curl", {{"coherence", "clump"}, {"clumpChannel", "lockId"}})});
    const auto channels = graphChannels(g.dump());
    const NodeChannels& small = channels.at("clumpSmall");
    EXPECT_TRUE(small.overwrites.empty());
    ASSERT_NE(incoming(small, "lockId"), nullptr);
    EXPECT_EQ(sourceOf(small, "lockId"), "clumpLarge");
    EXPECT_EQ(writtenNames(small), std::vector<std::string>{"clumpId"});
    EXPECT_EQ(small.writes.at(0).param, "clumpChannel");

    const NodeChannels& curl = channels.at("curl");
    ASSERT_NE(incoming(curl, "lockId"), nullptr);
    ASSERT_NE(incoming(curl, "clumpId"), nullptr);
    EXPECT_EQ(sourceOf(curl, "lockId"), "clumpLarge");
    EXPECT_EQ(sourceOf(curl, "clumpId"), "clumpSmall");
    const ChannelRead* r = read(curl, "clumpChannel");
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->reaches);
    EXPECT_EQ(r->node, "clumpLarge");
    EXPECT_TRUE(curl.writes.empty());
}

TEST(AttributeFlow, AClumpWritingTheNameOfAnUpstreamClumpOverwritesIt) {
    const json g = chainGraph({node("clump1", "Clump"), node("clump2", "Clump"), node("curl", "Curl", {{"coherence", "clump"}})});
    const auto channels = graphChannels(g.dump());
    EXPECT_TRUE(channels.at("clump1").overwrites.empty());
    const NodeChannels& second = channels.at("clump2");
    ASSERT_EQ(second.overwrites.size(), 1u);
    EXPECT_EQ(second.overwrites[0].name, "clumpId");
    EXPECT_EQ(second.overwrites[0].param, "clumpChannel");
    EXPECT_EQ(second.overwrites[0].node, "clump1");
    // The Curl after them finds the last one's.
    EXPECT_EQ(sourceOf(channels.at("curl"), "clumpId"), "clump2");
    // A Clump's regionChannel reads region, which nothing writes here.
    const ChannelRead* region = read(second, "regionChannel");
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->name, "region");
    EXPECT_FALSE(region->reaches);
    EXPECT_TRUE(region->optional);  // region exists only with a regionMap: the default missing is no mistake
}

TEST(AttributeFlow, ACurlReadingANameThatDoesNotReachItIsFlagged) {
    const json g = chainGraph({node("clump", "Clump"), node("curl", "Curl", {{"coherence", "clump"}, {"clumpChannel", "lockId"}}),
                               node("strandCurl", "Curl", {{"coherence", "strand"}, {"clumpChannel", "lockId"}})});
    const auto channels = graphChannels(g.dump());
    const ChannelRead* r = read(channels.at("curl"), "clumpChannel");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->name, "lockId");
    EXPECT_FALSE(r->reaches);
    EXPECT_FALSE(r->optional);
    EXPECT_TRUE(r->node.empty());
    // A Curl of coherence strand does not show (nor read) its clumpChannel.
    EXPECT_EQ(read(channels.at("strandCurl"), "clumpChannel"), nullptr);
    // Empty: none.
    const json none = chainGraph({node("curl", "Curl", {{"coherence", "clump"}, {"clumpChannel", ""}})});
    EXPECT_TRUE(graphChannels(none.dump()).at("curl").reads.empty());
}

TEST(AttributeFlow, WetWritesItsLevelsAndWetnessUnderFixedNames) {
    const json g = chainGraph({node("wet", "Wet", {{"levels", 3}}), node("color", "HairColor", {{"clumpChannel", "clumpId1"}})});
    const auto channels = graphChannels(g.dump());
    const NodeChannels& wet = channels.at("wet");
    EXPECT_EQ(writtenNames(wet), (std::vector<std::string>{"clumpId", "clumpId1", "clumpId2", "wetness"}));
    EXPECT_EQ(wet.writes[1].param, "clumpChannel");
    EXPECT_EQ(wet.writes[3].param, "");
    const ChannelRead* r = read(channels.at("color"), "clumpChannel");
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->reaches);
    EXPECT_EQ(r->node, "wet");
    // HairColor's own channels reach the Output after it.
    const json more = chainGraph({node("color", "HairColor"), node("flyaway", "Flyaway"), node("out", "Output")});
    const auto moreChannels = graphChannels(more.dump());
    const NodeChannels& out = moreChannels.at("out");
    EXPECT_EQ(sourceOf(out, "melanin"), "color");
    EXPECT_EQ(sourceOf(out, "colorR"), "color");
    EXPECT_EQ(sourceOf(out, "flyaway"), "flyaway");
    EXPECT_EQ(incoming(out, "_preview"), nullptr);  // a node's own: never listed
    // A Wet after a Clump overwrites its clumpId.
    const json both = chainGraph({node("clump", "Clump"), node("wet", "Wet")});
    const auto bothChannels = graphChannels(both.dump());
    const NodeChannels& wetAfter = bothChannels.at("wet");
    ASSERT_EQ(wetAfter.overwrites.size(), 1u);
    EXPECT_EQ(wetAfter.overwrites[0].node, "clump");
}

TEST(AttributeFlow, AnAttributeNodeWritesItsNameForAttributeMapsAfterIt) {
    const json g = chainGraph({node("mask", "Attribute", {{"name", "mask"}}),
                               node("clump", "Clump", {{"strength", {{"attribute", "mask"}, {"component", "r"}, {"min", 0.0}, {"max", 1.0}}}}),
                               node("noise", "Noise", {{"amplitude", {{"attribute", "other"}, {"component", "r"}, {"min", 0.0}, {"max", 1.0}}}}),
                               node("again", "Attribute", {{"name", "mask"}})});
    const auto channels = graphChannels(g.dump());
    EXPECT_EQ(writtenNames(channels.at("mask")), std::vector<std::string>{"mask"});
    const ChannelRead* strength = read(channels.at("clump"), "strength");
    ASSERT_NE(strength, nullptr);
    EXPECT_TRUE(strength->reaches);
    EXPECT_EQ(strength->node, "mask");
    const ChannelRead* amplitude = read(channels.at("noise"), "amplitude");
    ASSERT_NE(amplitude, nullptr);
    EXPECT_FALSE(amplitude->reaches);
    ASSERT_EQ(channels.at("again").overwrites.size(), 1u);
    EXPECT_EQ(channels.at("again").overwrites[0].node, "mask");
}

TEST(AttributeFlow, InterpolateWritesRegionsOnlyWithARegionMapAndCarriesGuideMaps) {
    const auto without = graphChannels(chainGraph({node("clump", "Clump")}).dump());
    EXPECT_TRUE(without.at("interpolate").writes.empty());
    EXPECT_FALSE(read(without.at("clump"), "regionChannel")->reaches);

    const auto with = graphChannels(chainGraph({node("clump", "Clump")}, {{"regionMap", "parts"}}).dump(), {"density"});
    EXPECT_EQ(writtenNames(with.at("interpolate")), std::vector<std::string>{"region"});
    const ChannelRead* region = read(with.at("clump"), "regionChannel");
    EXPECT_TRUE(region->reaches);
    EXPECT_EQ(region->node, "interpolate");
    // The guides' painted map, carried through the guides input.
    ASSERT_NE(incoming(with.at("clump"), "density"), nullptr);
    EXPECT_EQ(sourceOf(with.at("clump"), "density"), "guides");
    EXPECT_EQ(sourceOf(with.at("interpolate"), "density"), "guides");

    // The influence channel with a limitDistance only.
    const auto limited = graphChannels(chainGraph({}, {{"limitDistance", 1.0}}).dump());
    EXPECT_EQ(writtenNames(limited.at("interpolate")), std::vector<std::string>{"guideInfluence"});
}

TEST(AttributeFlow, BypassMergeSwitchGroupAndCompounds) {
    // A bypassed Clump writes nothing.
    json g = chainGraph({node("clump", "Clump"), node("curl", "Curl", {{"coherence", "clump"}})});
    g["nodes"][4]["bypass"] = true;
    auto channels = graphChannels(g.dump());
    EXPECT_TRUE(channels.at("clump").writes.empty());
    EXPECT_FALSE(read(channels.at("curl"), "clumpChannel")->reaches);

    // Merge hands on both inputs' channels; a Group combining with its name made before extends it.
    g = chainGraph({node("a", "Attribute", {{"name", "fromA"}})});
    g["nodes"].push_back(node("b", "Attribute", {{"name", "fromB"}}));
    g["nodes"].push_back(node("merge", "Merge"));
    g["nodes"].push_back(node("group1", "Group", {{"name", "fromA"}, {"combine", "add"}}));
    g["nodes"].push_back(node("group2", "Group", {{"name", "fromA"}}));
    g["connections"].push_back({{"from", "interpolate"}, {"to", "b"}, {"input", "strands"}});
    g["connections"].push_back({{"from", "a"}, {"to", "merge"}, {"input", "a"}});
    g["connections"].push_back({{"from", "b"}, {"to", "merge"}, {"input", "b"}});
    g["connections"].push_back({{"from", "merge"}, {"to", "group1"}, {"input", "strands"}});
    g["connections"].push_back({{"from", "group1"}, {"to", "group2"}, {"input", "strands"}});
    channels = graphChannels(g.dump());
    EXPECT_EQ(sourceOf(channels.at("group1"), "fromA"), "a");
    EXPECT_EQ(sourceOf(channels.at("group1"), "fromB"), "b");
    EXPECT_TRUE(channels.at("group1").overwrites.empty());
    EXPECT_EQ(channels.at("group2").overwrites.size(), 1u);  // replace: an overwrite

    // A compound writes what its inner nodes write, under its own name, and reports their overwrites.
    json inner = {{"version", 1},
                  {"nodes", json::array({{{"name", "in"}, {"type", "Input"}, {"params", {{"name", "strands"}, {"type", "Strands"}}}},
                                         node("innerClump", "Clump")})},
                  {"connections", json::array({{{"from", "in"}, {"to", "innerClump"}, {"input", "strands"}}})},
                  {"output", "innerClump"}};
    json definition = {{"graph", inner}, {"inputs", json::array({{{"name", "strands"}, {"type", "Strands"}}})}};
    g = chainGraph({node("clump", "Clump"), node("look", "Compound", {{"definition", definition}}),
                    node("curl", "Curl", {{"coherence", "clump"}})});
    channels = graphChannels(g.dump());
    EXPECT_EQ(writtenNames(channels.at("look")), std::vector<std::string>{"clumpId"});
    ASSERT_EQ(channels.at("look").overwrites.size(), 1u);
    EXPECT_EQ(channels.at("look").overwrites[0].param, "innerClump.clumpChannel");
    EXPECT_EQ(channels.at("look").overwrites[0].node, "clump");
    EXPECT_EQ(read(channels.at("curl"), "clumpChannel")->node, "look");
}

TEST(AttributeFlow, AnUnfinishedGraphReadsAsItIsAndTheJsonFormMatches) {
    // No output, an input unconnected: nothing reaches the loose Curl.
    const json g = {{"nodes", json::array({node("curl", "Curl", {{"coherence", "clump"}}), node("weird", "NoSuchType")})}};
    const auto channels = graphChannels(g.dump());
    EXPECT_TRUE(channels.at("curl").incoming.empty());
    EXPECT_FALSE(read(channels.at("curl"), "clumpChannel")->reaches);
    EXPECT_TRUE(channels.at("weird").writes.empty());
    EXPECT_THROW(graphChannels("[1, 2]"), GraphError);

    const json two = chainGraph({node("clump1", "Clump"), node("clump2", "Clump")});
    const json out = json::parse(GraphEdit(two.dump()).channelsJson());
    EXPECT_EQ(out.at("clump2").at("overwrites").at(0).at("node"), "clump1");
    EXPECT_EQ(out.at("clump2").at("incoming").at(0).at("name"), "clumpId");
    EXPECT_EQ(out.at("clump2").at("writes").at(0).at("param"), "clumpChannel");
    EXPECT_EQ(out.at("clump2").at("reads").at(0).at("reaches"), false);
}

}  // namespace
}  // namespace groomlab
