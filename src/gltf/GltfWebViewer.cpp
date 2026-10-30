#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "gltf/GltfInternal.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <string>
#include <system_error>

namespace lila::gltf
{
namespace
{
constexpr const char* kModelViewerUrl = "https://cdn.jsdelivr.net/npm/@google/model-viewer@3.5.0/dist/model-viewer.min.js";

std::string htmlEscape(std::string_view s)
{
    std::string out;
    for (char c : s)
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    return out;
}

/** The viewport's HDRI as a file, or empty for the procedural studio; mirrors ui::environments::resolve. */
fs::path viewportHdri()
{
    const std::string pref = PrefStore::get().value<std::string>("general", "/viewport/env/path", "");
    if (pref == "procedural")
        return {};
    fs::path wanted = pref.empty() ? fs::path("studio_small_09_2k.hdr") : utf8Path(pref);
    if (wanted.is_relative())
        wanted = paths::resourcesDir() / "hdri" / wanted;
    std::error_code ec;
    return fs::is_regular_file(wanted, ec) ? wanted : fs::path();
}

glm::dvec3 viewportBackground()
{
    json bg = PrefStore::get().value("general", "/viewport/background");
    if (bg.is_array() && bg.size() == 3 && bg[0].is_number())
        return {bg[0].get<double>(), bg[1].get<double>(), bg[2].get<double>()};
    return {0.3, 0.3, 0.3};
}

std::string cssColor(const glm::dvec3& srgb)
{
    auto byte = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    return std::format("#{:02x}{:02x}{:02x}", byte(srgb.r), byte(srgb.g), byte(srgb.b));
}

struct PageSettings
{
    std::string title, author;
    bool autoRotate = true;
    glm::dvec3 background{0.3};
    double exposureStops = 0.0;
    std::string toneMapping = "neutral";
    std::string environmentImage = "neutral";
};

std::string viewerHtml(const PageSettings& s)
{
    const std::string title = htmlEscape(s.title);
    const std::string author = htmlEscape(s.author);
    std::string caption;
    if (!title.empty() || !author.empty())
    {
        caption = "  <div class=\"caption\">";
        if (!title.empty())
            caption += "<div class=\"title\">" + title + "</div>";
        if (!author.empty())
            caption += "<div class=\"author\">" + author + "</div>";
        caption += "</div>\n";
    }
    return std::format(R"(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{0}</title>
<script type="module" src="{1}"></script>
<style>
  html, body {{ margin: 0; height: 100%; background: {2}; color: #e6e6e6; font: 13px system-ui, -apple-system, "Segoe UI", sans-serif; overflow: hidden; }}
  model-viewer {{ width: 100%; height: 100%; background: {2}; --poster-color: transparent; }}
  .caption {{ position: absolute; left: 16px; bottom: 16px; pointer-events: none; text-shadow: 0 1px 3px rgba(0,0,0,.6); }}
  .title {{ font-size: 16px; font-weight: 600; }}
  .author {{ opacity: .75; margin-top: 2px; }}
  .controls {{ position: absolute; right: 16px; bottom: 16px; display: flex; gap: 6px; }}
  .controls button {{ background: rgba(20,20,22,.7); color: #e6e6e6; border: 1px solid rgba(255,255,255,.15); border-radius: 4px; padding: 5px 10px; font: inherit; cursor: pointer; }}
  .controls button[aria-pressed="true"] {{ border-color: rgba(255,255,255,.5); }}
</style>
</head>
<body>
<model-viewer id="viewer" src="model.glb" alt="{0}" camera-controls touch-action="pan-y" {3}environment-image="{4}" exposure="{5}" tone-mapping="{6}" shadow-intensity="1" interaction-prompt="none">
{7}  <div class="controls"><button id="rotate" aria-pressed="{8}">Turntable</button></div>
</model-viewer>
<script>
  const viewer = document.getElementById("viewer");
  const rotate = document.getElementById("rotate");
  rotate.addEventListener("click", () => {{
    const on = !viewer.hasAttribute("auto-rotate");
    viewer.toggleAttribute("auto-rotate", on);
    rotate.setAttribute("aria-pressed", String(on));
  }});
</script>
</body>
</html>
)",
                       title.empty() ? "Model" : title, kModelViewerUrl, cssColor(s.background), s.autoRotate ? "auto-rotate " : "",
                       s.environmentImage, std::format("{:.4g}", std::exp2(s.exposureStops)), s.toneMapping, caption,
                       s.autoRotate ? "true" : "false");
}
} // namespace

void registerWebViewerCommand()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "exportWebViewer",
        .doc = "Write a folder to upload to a website for showing a model interactively: model.glb (exported as by exportGltf, "
               "materials and textures embedded), index.html (Google's <model-viewer> loaded from a CDN, with orbit controls, "
               "turntable and a caption) and environment.hdr (the viewport's HDRI).\n\n"
               "The page must be served over http(s); browsers block model.glb when index.html is opened from disk. Returns the folder.",
        .positionalDoc = "folder",
        .flags = {{"selection", "sl", T::Bool, nullptr, "Export the selection; False exports the whole scene. Default: the selection when there is one."},
                  {"title", "t", T::String, "", "Caption title, also the page title."},
                  {"author", "au", T::String, "", "Caption line under the title."},
                  {"autoRotate", "ar", T::Bool, true, "Start with the turntable spinning."},
                  {"background", "bg", T::Vec3, nullptr, "Page background, sRGB 0 to 1; the viewport background by default."},
                  {"exposure", "ex", T::Float, nullptr, "Exposure in stops; the viewport exposure by default."},
                  {"tonemapper", "tm", T::String, "neutral", "Tone mapping: neutral, aces or agx.", {"neutral", "aces", "agx"}},
                  {"environment", "env", T::Bool, true, "Light with the viewport's HDRI (copied as environment.hdr); off uses model-viewer's neutral lighting."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string() || a.positional[0].get<std::string>().empty())
                throw CommandError("exportWebViewer: folder required");
            const std::string folderText = a.positional[0].get<std::string>();
            const fs::path folder = utf8Path(folderText);
            std::error_code ec;
            fs::create_directories(folder, ec);
            if (!fs::is_directory(folder, ec))
                throw CommandError("exportWebViewer: can't create folder '" + folderText + "'");

            scene::Graph& g = scene::Graph::current();
            const bool selection = a.has("selection") ? a.get<bool>("selection") : !g.selection().empty();
            if (selection && g.selection().empty())
                throw CommandError("exportWebViewer: nothing selected");

            PageSettings page;
            page.toneMapping = a.get<std::string>("tonemapper", "neutral");
            if (page.toneMapping != "neutral" && page.toneMapping != "aces" && page.toneMapping != "agx")
                throw CommandError("exportWebViewer: tonemapper must be neutral, aces or agx");

            ops::execute("exportGltf", {{"args", {utf8(folder / "model.glb")}}, {"binary", true}, {"selectionOnly", selection}});

            page.title = a.get<std::string>("title", "");
            page.author = a.get<std::string>("author", "");
            page.autoRotate = a.get<bool>("autoRotate", true);
            page.exposureStops = a.has("exposure") ? a.get<double>("exposure") : PrefStore::get().value<double>("general", "/viewport/exposure", 0.0);
            page.background = viewportBackground();
            if (a.has("background"))
            {
                const json& bg = a.flags["background"];
                if (!bg.is_array() || bg.size() != 3)
                    throw CommandError("exportWebViewer: background must be three numbers");
                page.background = {bg[0].get<double>(), bg[1].get<double>(), bg[2].get<double>()};
            }

            const fs::path envFile = folder / "environment.hdr";
            fs::remove(envFile, ec);
            if (a.get<bool>("environment", true))
            {
                const fs::path hdri = viewportHdri();
                std::string ext = utf8(hdri.extension());
                std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (hdri.empty())
                    log::info("exportWebViewer: the viewport uses the procedural studio; the page uses neutral lighting");
                else if (ext != ".hdr")
                    log::warn("exportWebViewer: {} isn't a .hdr, which model-viewer needs; the page uses neutral lighting", utf8(hdri));
                else if (!fs::copy_file(hdri, envFile, fs::copy_options::overwrite_existing, ec))
                    log::warn("exportWebViewer: couldn't copy {}: {}", utf8(hdri), ec.message());
                else
                    page.environmentImage = "environment.hdr";
            }

            if (!writeFile(folder / "index.html", viewerHtml(page)))
                throw CommandError("exportWebViewer: can't write index.html");
            return folderText;
        },
        .examples = {"lila.ops.exportWebViewer(\"C:/Portfolio/Ship\", title=\"Ship\", author=\"Jared Taylor\")"},
    });
}
} // namespace lila::gltf
