#include "ui/BakerPanel.h"

#include "bake/Bake.h"
#include "bake/BakeNodes.h"
#include "core/ImageIO.h"
#include "mcp/McpServer.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/AgentCapture.h"

#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <format>

namespace lila::ui
{
namespace
{

json imageBlock(const QImage& img)
{
    QByteArray b64 = QByteArray::fromStdString(capture::png(img)).toBase64();
    return {{"type", "image"}, {"data", b64.toStdString()}, {"mimeType", "image/png"}};
}

json textBlock(const std::string& text) { return {{"type", "text"}, {"text", text}}; }

std::string percent(double part, double whole) { return std::format("{:.2f}%", whole > 0.0 ? 100.0 * part / whole : 0.0); }

/** A bake result as lines an agent can act on. */
std::string summary(const json& r)
{
    const json stats = r.value("stats", json::object());
    const double samples = stats.value("samples", 0.0);
    std::string out;
    if (r.value("dryRun", false))
        out += std::format("DRY RUN: {} files planned\n", r.value("outputs", json::array()).size());
    else
        out += std::format("{} files in {:.2f} s | {} samples, {} hits, {} missed ({}), {} backfaces\n", r.value("outputs", json::array()).size(),
                           r.value("seconds", 0.0), stats.value("samples", 0), stats.value("hits", 0), stats.value("misses", 0),
                           percent(stats.value("misses", 0.0), samples), stats.value("backfaces", 0));
    for (const json& g : stats.value("groups", json::array()))
    {
        const double s = g.value("samples", 0.0);
        out += std::format("  group {}: {} low tris, {} high tris, missed {}, backfaces {}{}\n", g.value("group", std::string()),
                           g.value("lowTriangles", 0), g.value("highTriangles", 0), percent(g.value("misses", 0.0), s), g.value("backfaces", 0),
                           s > 0.0 && g.value("misses", 0.0) / s > 0.01 ? "  <- raise frontal/rear (bakeGroup(group, frontal=, rear=)) or cageOffset" : "");
    }
    for (const json& o : r.value("outputs", json::array()))
    {
        out += std::format("  {} udim {} {}x{} {} bit {}: {}", o.value("map", std::string()), o.value("udim", 1001), o.value("width", 0),
                           o.value("height", 0), o.value("bits", 8), o.value("format", std::string()), o.value("file", std::string()));
        if (const json meta = o.value("meta", json::object()); meta.is_object() && !meta.empty())
            out += " meta " + meta.dump();
        out += "\n";
    }
    for (const json& w : r.value("warnings", json::array()))
        if (w.is_string())
            out += "  WARNING: " + w.get<std::string>() + "\n";
    if (r.contains("skipped") && !r["skipped"].empty())
        out += "  skipped (not implemented): " + r["skipped"].dump() + "\n";
    if (r.contains("manifest") && r["manifest"].is_string())
        out += "  manifest: " + r["manifest"].get<std::string>() + "\n";
    return out;
}

/** project= when given, as the commands take it. */
json withProject(const json& args, json call = json::object())
{
    if (args.contains("project") && args["project"].is_string())
        call["project"] = args["project"];
    return call;
}

json lastResultFor(const json& args)
{
    json status = ops::execute("bakeStatus", withProject(args));
    if (status.contains("projects"))
        throw std::runtime_error("several bake projects; pass project");
    return status.value("last", json());
}

/** The texel a uv or texel argument names in an output, or false when outside. */
bool texelOf(const json& args, int w, int h, int& x, int& y)
{
    if (args.contains("texel") && args["texel"].is_array() && args["texel"].size() == 2)
    {
        x = args["texel"][0].get<int>();
        y = args["texel"][1].get<int>();
    }
    else if (args.contains("uv") && args["uv"].is_array() && args["uv"].size() == 2)
    {
        const double u = args["uv"][0].get<double>(), v = args["uv"][1].get<double>();
        // Row 0 is the top scanline: v grows upwards.
        x = int(std::floor((u - std::floor(u)) * w));
        y = int(std::floor((1.0 - (v - std::floor(v))) * h));
    }
    else
        return false;
    return x >= 0 && y >= 0 && x < w && y < h;
}
} // namespace

void registerBakerMcpTools()
{
    mcp::Server& s = mcp::Server::get();

    s.addTool({
        .name = "bake",
        .description =
            "Bake texture maps from high to low meshes and look at them. With `target` (substancePainter, unreal, or any from bakeTargets) "
            "it is the one-click Bake for Target: applies the target's maps and file names, auto-groups _low/_high meshes when the project has "
            "no groups, bakes everything. Without it, bakes the project's enabled maps (or `maps`) for its groups (or `groups`). Returns a "
            "summary (files, per-group miss rates with what to raise when rays miss, warnings) and a contact sheet of every written map. "
            "Set groups up first with run_command bakeGroup / autoGroupBake; use bake_lint before and bake_inspect after.",
        .inputSchema = {{"type", "object"},
                        {"properties",
                         {{"project", {{"type", "string"}, {"description", "Bake project; default the scene's only one (created by target bakes)."}}},
                          {"target", {{"type", "string"}, {"description", "Target profile for a one-click bake."}}},
                          {"optional", {{"type", "boolean"}, {"description", "With target: also bake its optional maps."}}},
                          {"groups", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Only these bake groups."}}},
                          {"maps", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Only these map ids (bakeMaps)."}}},
                          {"dry_run", {{"type", "boolean"}, {"description", "Plan the files without baking."}}},
                          {"backend", {{"type", "string"}, {"description", "cpu (default) or a GPU backend when registered."}}},
                          {"thumbnails", {{"type", "boolean"}, {"description", "Return the contact sheet image (default true)."}}},
                          {"tile", {{"type", "integer"}, {"description", "Contact sheet tile size in px (default 160)."}}}}}},
        .runContent = [](const json& args) {
            json call = withProject(args, {{"dryRun", args.value("dry_run", false)}, {"backend", args.value("backend", std::string("cpu"))}});
            json r;
            if (args.contains("target") && args["target"].is_string())
            {
                call["target"] = args["target"];
                call["optional"] = args.value("optional", false);
                r = ops::execute("bakeForTarget", call, CallSource::UI);
            }
            else
            {
                if (args.contains("groups"))
                    call["groups"] = args["groups"];
                if (args.contains("maps"))
                    call["maps"] = args["maps"];
                r = ops::execute("bake", call, CallSource::UI);
            }
            json content = json::array({textBlock(summary(r))});
            if (args.value("thumbnails", true) && !r.value("dryRun", false))
                if (QImage sheet = bakeContactSheet(r, std::clamp(args.value("tile", 160), 32, 512)); !sheet.isNull())
                    content.push_back(imageBlock(sheet));
            return content;
        },
    });

    s.addTool({
        .name = "bake_lint",
        .description = "Check a bake setup before baking: hard edges off UV seams, overlapping or missing UVs, texel density outliers, "
                       "mirrored islands, lows with no high in range, cage problems (bakeLint), plus the planned files and their warnings "
                       "from a dry run. Fix what it reports, then bake.",
        .inputSchema = {{"type", "object"}, {"properties", {{"project", {{"type", "string"}, {"description", "Bake project; default the only one."}}}}}},
        .run = [](const json& args) {
            std::string out;
            if (ops::find("bakeLint"))
                out += "bakeLint: " + ops::execute("bakeLint", withProject(args)).dump(2) + "\n";
            else
                out += "bakeLint is not available in this build; only the dry run below was checked.\n";
            const json status = ops::execute("bakeStatus", withProject(args));
            if (status.contains("projects"))
                return out + "Several bake projects: pass project. " + status.dump();
            for (const json& g : status.value("groups", json::array()))
            {
                if (g.value("lows", json::array()).empty())
                    out += "  group " + g.value("name", std::string()) + " has no lows\n";
                if (g.value("highs", json::array()).empty())
                    out += "  group " + g.value("name", std::string()) + " has no highs (add one, or self=True with a preset)\n";
            }
            try
            {
                out += summary(ops::execute("bake", withProject(args, {{"dryRun", true}})));
            }
            catch (const CommandError& e)
            {
                out += std::string("  dry run failed: ") + e.what() + "\n";
            }
            return out;
        },
    });

    s.addTool({
        .name = "bake_inspect",
        .description = "Read the last bake's maps: the RGBA value of every map at a `uv` ([u, v], v up) or `texel` ([x, y], row 0 at the top), "
                       "and with `map` plus image=true that map as an image with its per-channel min/mean/max. Use it to confirm a fix "
                       "(misses gone, normals right, IDs exact) without leaving the agent loop.",
        .inputSchema = {{"type", "object"},
                        {"properties",
                         {{"project", {{"type", "string"}}},
                          {"map", {{"type", "string"}, {"description", "Map id (normal, height, id, ao, ...); default every output."}}},
                          {"udim", {{"type", "integer"}}},
                          {"uv", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                          {"texel", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
                          {"image", {{"type", "boolean"}, {"description", "Return the map (needs map) as an image."}}},
                          {"size", {{"type", "integer"}, {"description", "Image size in px (default 512)."}}}}}},
        .runContent = [](const json& args) {
            json content = json::array();
            std::string text;
            if (ops::find("bakeInspect"))
            {
                json call = args;
                call.erase("image");
                call.erase("size");
                try
                {
                    text += "bakeInspect: " + ops::execute("bakeInspect", call).dump(2) + "\n";
                }
                catch (const CommandError& e)
                {
                    text += std::string("bakeInspect: ") + e.what() + "\n";
                }
            }
            const json last = lastResultFor(args);
            if (!last.is_object())
                throw std::runtime_error("no bake this session; run bake first");
            const std::string map = args.value("map", std::string());
            QImage shown;
            for (const json& o : last.value("outputs", json::array()))
            {
                if (!map.empty() && o.value("map", std::string()) != map)
                    continue;
                if (args.contains("udim") && o.value("udim", 1001) != args["udim"].get<int>())
                    continue;
                const std::string file = o.value("file", std::string());
                std::string err;
                const image::Image img = image::read(std::filesystem::path(file), &err);
                if (img.width <= 0)
                {
                    text += std::format("{}: can't read {}: {}\n", o.value("map", std::string()), file, err);
                    continue;
                }
                text += std::format("{} udim {} ({}x{}) {}\n", o.value("map", std::string()), o.value("udim", 1001), img.width, img.height, file);
                int x = 0, y = 0;
                if (texelOf(args, img.width, img.height, x, y))
                {
                    const float* p = &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4];
                    text += std::format("  texel [{}, {}]: rgba [{:.4f}, {:.4f}, {:.4f}, {:.4f}] (8-bit [{}, {}, {}, {}])\n", x, y, p[0], p[1], p[2], p[3],
                                        int(std::lround(std::clamp(p[0], 0.0f, 1.0f) * 255.0f)), int(std::lround(std::clamp(p[1], 0.0f, 1.0f) * 255.0f)),
                                        int(std::lround(std::clamp(p[2], 0.0f, 1.0f) * 255.0f)), int(std::lround(std::clamp(p[3], 0.0f, 1.0f) * 255.0f)));
                }
                if (!map.empty())
                {
                    float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
                    double mean[4] = {0.0, 0.0, 0.0, 0.0};
                    const size_t n = size_t(img.width) * size_t(img.height);
                    for (size_t i = 0; i < n; ++i)
                        for (int c = 0; c < 4; ++c)
                        {
                            const float v = img.rgba[i * 4 + size_t(c)];
                            lo[c] = std::min(lo[c], v);
                            hi[c] = std::max(hi[c], v);
                            mean[c] += v;
                        }
                    for (int c = 0; c < 4; ++c)
                        text += std::format("  {}: min {:.4f} mean {:.4f} max {:.4f}\n", "RGBA"[c], lo[c], mean[c] / double(n), hi[c]);
                    if (const json meta = o.value("meta", json::object()); !meta.empty())
                        text += "  meta " + meta.dump() + "\n";
                    if (args.value("image", false) && shown.isNull())
                        shown = bakeThumbnail(std::filesystem::path(file), std::clamp(args.value("size", 512), 32, 2048));
                }
            }
            if (text.empty())
                text = "no output matches";
            content.push_back(textBlock(text));
            if (!shown.isNull())
                content.push_back(imageBlock(shown));
            return content;
        },
    });
}
} // namespace lila::ui
