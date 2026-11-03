#pragma once

#include "core/ImageIO.h"
#include "ops/Command.h"
#include "tex/Evaluate.h"
#include "tex/Graph.h"
#include "tex/Plan.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace textest
{
using namespace lila;
using namespace lila::tex;

inline GNode& add(Graph& g, const std::string& name, const std::string& type, json values = json::object())
{
    g.nodes.push_back({name, type, {0.0, 0.0}, std::move(values)});
    return g.nodes.back();
}

/** "A.out" -> "B.in", replacing what fed B.in. */
inline void link(Graph& g, const std::string& from, const std::string& to)
{
    auto f = from.find('.'), t = to.find('.');
    std::erase_if(g.links, [&](const GLink& l) { return l.toNode == to.substr(0, t) && l.toSocket == to.substr(t + 1); });
    g.links.push_back({from.substr(0, f), from.substr(f + 1), to.substr(0, t), to.substr(t + 1)});
}

inline Plan plan(const Graph& g, const std::string& node, const std::string& socket, int res, PortType type = PortType::Color)
{
    PlanRequest r;
    r.width = r.height = res;
    r.outputs = {{"out", node, socket, type, {}}};
    return buildPlan(g, r);
}

inline image::Image eval(const Graph& g, const std::string& node, const std::string& socket, int res, PortType type = PortType::Color)
{
    Plan p = plan(g, node, socket, res, type);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    return evaluateCpu(p).outputs.at("out");
}

inline const float* texel(const image::Image& img, int x, int y) { return &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4]; }

inline image::Image downsample(const image::Image& img, int factor)
{
    image::Image out{img.width / factor, img.height / factor, {}};
    out.rgba.assign(size_t(out.width) * size_t(out.height) * 4, 0.0f);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
            for (int c = 0; c < 4; ++c)
            {
                float s = 0.0f;
                for (int j = 0; j < factor; ++j)
                    for (int i = 0; i < factor; ++i)
                        s += texel(img, x * factor + i, y * factor + j)[c];
                out.rgba[(size_t(y) * out.width + x) * 4 + c] = s / float(factor * factor);
            }
    return out;
}

/** Mean absolute difference over RGB. */
inline double meanDiff(const image::Image& a, const image::Image& b)
{
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    double s = 0.0;
    for (size_t i = 0; i < a.rgba.size(); ++i)
        if (i % 4 != 3)
            s += std::abs(double(a.rgba[i]) - double(b.rgba[i]));
    return s / double(a.rgba.size() / 4 * 3);
}

inline double maxDiff(const image::Image& a, const image::Image& b)
{
    REQUIRE(a.rgba.size() == b.rgba.size());
    double m = 0.0;
    for (size_t i = 0; i < a.rgba.size(); ++i)
        m = std::max(m, std::abs(double(a.rgba[i]) - double(b.rgba[i])));
    return m;
}

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

inline json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }

/** g with `node.socket` into a fresh Material Output's baseColor. */
inline Graph into(Graph g, const std::string& from)
{
    g.nodes.push_back({"Output", kMaterialOutputType, {0.0, 0.0}, json::object()});
    link(g, from, "Output.baseColor");
    return g;
}

/** An emptied temp folder for one test. */
inline std::filesystem::path tempDir(const std::string& name)
{
    std::filesystem::path d = std::filesystem::temp_directory_path() / "lila_tex_tests" / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

inline std::filesystem::path goldenDir() { return std::filesystem::path(LILA_SOURCE_DIR) / "tests" / "data" / "tex"; }

/** Compares against tests/data/tex/<name>.png (16 bit); writes it when missing or LILA_TEX_UPDATE_GOLDENS is set. */
inline void golden(const std::string& name, const image::Image& img)
{
    std::filesystem::path path = goldenDir() / (name + ".png");
    if (std::getenv("LILA_TEX_UPDATE_GOLDENS") || !std::filesystem::exists(path))
    {
        std::filesystem::create_directories(goldenDir());
        image::write(path, img, 16);
        MESSAGE("wrote golden " << path.string());
        return;
    }
    std::string err;
    image::Image ref = image::readPng(path, &err);
    REQUIRE_MESSAGE(ref.width == img.width, err);
    // PNG holds 0 to 1; the evaluator's values clamp the same way before comparing.
    image::Image clamped = img;
    for (float& v : clamped.rgba)
        v = std::clamp(v, 0.0f, 1.0f);
    CAPTURE(name);
    CHECK(maxDiff(clamped, ref) < 2e-3);
}
} // namespace textest
