#include "TexTest.h"

#include "tex/SlangRt.h"
#include "tex/TextureSet.h"

#include <algorithm>

using namespace textest;

namespace
{
Graph noiseLevels()
{
    Graph g = Graph::makeMaterial("Set");
    add(g, "Noise", "perlin", {{"scale", 4}});
    add(g, "Lv", "levels", {{"gamma", 1.5}});
    link(g, "Noise.out", "Lv.input");
    link(g, "Lv.out", "Output.baseColor");
    return g;
}

size_t countKind(const Plan& p, PassKind k)
{
    return size_t(std::count_if(p.passes.begin(), p.passes.end(), [&](const Pass& x) { return x.kind == k; }));
}
} // namespace

TEST_CASE("tex: codegen is deterministic")
{
    Graph g = noiseLevels();
    Plan a = plan(g, "Output", "baseColor", 64);
    Plan b = plan(Graph::fromJson(g.toJson()), "Output", "baseColor", 64);
    REQUIRE(a.error.empty());
    REQUIRE(a.passes.size() == b.passes.size());
    for (size_t i = 0; i < a.passes.size(); ++i)
        CHECK(a.passes[i].source == b.passes[i].source);
    CHECK(a.structureHash == b.structureHash);
    CHECK(a.uniforms == b.uniforms);
    // Helpers and type functions are emitted once per pass.
    const std::string& src = a.passes.back().source;
    CHECK(src.find("float noiseFbm(") == src.rfind("float noiseFbm("));
    CHECK(src.find("[shader(\"compute\")]") != std::string::npos);
}

TEST_CASE("tex: value edits keep the structure, link edits change it")
{
    Graph g = noiseLevels();
    Plan base = plan(g, "Output", "baseColor", 64);

    Graph edited = g;
    edited.node("Noise")->values["scale"] = 9;
    edited.node("Lv")->values["gamma"] = 0.5;
    edited.node("Output")->values["roughness"] = 0.1;
    Plan valueEdit = plan(edited, "Output", "baseColor", 64);
    CHECK(valueEdit.structureHash == base.structureHash);
    for (size_t i = 0; i < base.passes.size(); ++i)
        CHECK(valueEdit.passes[i].source == base.passes[i].source);
    CHECK(valueEdit.uniforms != base.uniforms);

    // Renaming a node changes nothing that compiles.
    Graph renamed = Graph::fromJson(json::parse([&] {
        std::string s = g.toJson().dump();
        for (size_t at; (at = s.find("\"Noise\"")) != std::string::npos;)
            s.replace(at, 7, "\"Grain\"");
        return s;
    }()));
    CHECK(plan(renamed, "Output", "baseColor", 64).structureHash == base.structureHash);

    Graph relinked = g;
    relinked.links.erase(relinked.links.begin());
    CHECK(plan(relinked, "Output", "baseColor", 64).structureHash != base.structureHash);
}

TEST_CASE("tex: the scheduler fuses chains and buffers fan-out, heavy taps, Buffer and native nodes")
{
    SUBCASE("a chain is one pass")
    {
        Plan p = plan(noiseLevels(), "Output", "baseColor", 64);
        REQUIRE(p.error.empty());
        CHECK(p.passes.size() == 1);
        CHECK(p.buffers.size() == 1);
        CHECK(p.buffers[0].kind == BufferKind::Output);
    }
    SUBCASE("an expensive node read twice is buffered once")
    {
        Graph g = noiseLevels();
        link(g, "Noise.out", "Output.roughness");
        PlanRequest r;
        r.width = r.height = 64;
        r.outputs = materialRequests(g, {{"baseColor", PortType::Color}, {"roughness", PortType::Gray}});
        Plan p = buildPlan(g, r);
        REQUIRE(p.error.empty());
        // Noise into a buffer (which doubles as the roughness output), then baseColor reading it.
        CHECK(p.passes.size() == 2);
        CHECK(p.passes[0].node == "Noise");
        CHECK(p.passes[1].reads == std::vector<int>{p.passes[0].output});
        CHECK(p.outputs.at("roughness") == p.passes[0].output);
    }
    SUBCASE("a cheap node read twice is duplicated")
    {
        Graph g = Graph::makeMaterial("Set");
        add(g, "C", "constant", {{"value", 0.3}});
        link(g, "C.out", "Output.roughness");
        link(g, "C.out", "Output.metallic");
        PlanRequest r;
        r.width = r.height = 16;
        r.outputs = materialRequests(g, {{"roughness", PortType::Gray}, {"metallic", PortType::Gray}});
        Plan p = buildPlan(g, r);
        CHECK(p.passes.size() == 2);
        CHECK(std::all_of(p.passes.begin(), p.passes.end(), [](const Pass& x) { return x.reads.empty(); }));
    }
    SUBCASE("multi-tap inputs over budget buffer their source")
    {
        Graph g = Graph::makeMaterial("Set");
        add(g, "Noise", "perlin");
        add(g, "N", "heightToNormal");
        link(g, "Noise.out", "N.height");
        link(g, "N.normal", "Output.normal");
        Plan fused = plan(g, "Output", "normal", 64);
        CHECK(fused.passes.size() == 1);
        PlanRequest r;
        r.width = r.height = 64;
        r.outputs = {{"out", "Output", "normal", PortType::Color, {}}};
        r.tapBudget = 100.0;
        Plan split = buildPlan(g, r);
        CHECK(split.passes.size() == 2);
        CHECK(split.passes[0].node == "Noise");
    }
    SUBCASE("a Buffer node is its own pass")
    {
        Graph g = noiseLevels();
        add(g, "Buf", "buffer");
        link(g, "Noise.out", "Buf.input");
        link(g, "Buf.out", "Lv.input");
        Plan p = plan(g, "Output", "baseColor", 64);
        REQUIRE(p.error.empty());
        CHECK(p.passes.size() == 2);
        CHECK(p.passes[0].node == "Buf");
    }
    SUBCASE("native nodes run as opaque passes on buffered inputs")
    {
        Graph g = noiseLevels();
        add(g, "Blur", "blur", {{"radius", 0.02}});
        link(g, "Noise.out", "Blur.input");
        link(g, "Blur.out", "Lv.input");
        Plan p = plan(g, "Output", "baseColor", 64);
        REQUIRE(p.error.empty());
        CHECK(countKind(p, PassKind::Native) == 1);
        CHECK(p.passes.size() == 3);
        const Pass& blur = p.passes[1];
        CHECK(blur.nativeType == "blur");
        CHECK(blur.inputBuffers.at("input") == p.passes[0].output);
        CHECK(blur.paramSlots.contains("radius"));
    }
    SUBCASE("the pass budget splits long chains")
    {
        Graph g = Graph::makeMaterial("Set");
        std::string prev;
        for (int i = 0; i < 6; ++i)
        {
            std::string n = "W" + std::to_string(i);
            add(g, n, "warp");
            add(g, "Noise" + std::to_string(i), "perlin");
            link(g, "Noise" + std::to_string(i) + ".out", n + ".warp");
            if (!prev.empty())
                link(g, prev + ".out", n + ".input");
            prev = n;
        }
        link(g, prev + ".out", "Output.baseColor");
        PlanRequest r;
        r.width = r.height = 32;
        r.outputs = {{"out", "Output", "baseColor", PortType::Color, {}}};
        r.passBudget = 400.0;
        Plan p = buildPlan(g, r);
        REQUIRE(p.error.empty());
        CHECK(p.passes.size() > 1);
        for (const Pass& pass : p.passes)
            CHECK(pass.cost <= 400.0);
    }
}

TEST_CASE("tex: plan errors name the problem")
{
    Graph g = Graph::makeMaterial("Set");
    add(g, "X", "noSuchNode");
    link(g, "X.out", "Output.baseColor");
    CHECK(plan(g, "Output", "baseColor", 16).error.find("noSuchNode") != std::string::npos);

    Graph cyc = Graph::makeMaterial("Set");
    add(cyc, "A", "levels");
    add(cyc, "B", "levels");
    link(cyc, "A.out", "B.input");
    link(cyc, "B.out", "A.input");
    link(cyc, "B.out", "Output.baseColor");
    CHECK(plan(cyc, "Output", "baseColor", 16).error.find("cycle") != std::string::npos);
}

TEST_CASE("tex: generated passes compile to SPIR-V for the GPU evaluator")
{
    Graph g = noiseLevels();
    add(g, "V", "voronoi");
    add(g, "Bl", "blend", {{"mode", "multiply"}});
    add(g, "Br", "bricks");
    add(g, "N", "heightToNormal");
    link(g, "V.edge", "Bl.foreground");
    link(g, "Lv.out", "Bl.background");
    link(g, "Br.mask", "Bl.mask");
    link(g, "Bl.out", "Output.baseColor");
    link(g, "Br.mask", "N.height");
    link(g, "N.normal", "Output.normal");
    PlanRequest r;
    r.width = r.height = 64;
    r.outputs = materialRequests(g, {{"baseColor", PortType::Color}, {"normal", PortType::Color}});
    Plan p = buildPlan(g, r);
    REQUIRE(p.error.empty());
    for (const Pass& pass : p.passes)
    {
        if (pass.kind != PassKind::Fused)
            continue;
        std::string err;
        std::vector<uint32_t> code = slangrt::spirv(pass.source, &err);
        CAPTURE(err);
        REQUIRE_FALSE(code.empty());
        CHECK(code[0] == 0x07230203u);
    }
}
