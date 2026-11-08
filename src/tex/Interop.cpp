#include "tex/Interop.h"

#include "bake/Bake.h"
#include "bake/BakePipeline.h"
#include "bake/Naming.h"
#include "core/ImageIO.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/TextureDeformers.h"
#include "scene/Node.h"
#include "tex/Evaluate.h"
#include "tex/Layers.h"
#include "tex/PaintStrokes.h"
#include "tex/Ports.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace lila::tex
{
namespace fs = std::filesystem;

namespace
{
scene::Node& setArg(const Args& a, const char* cmd)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError(std::string(cmd) + ": texture set required");
    scene::Node* n = findSet(scene::Graph::current(), a.positional[0].get<std::string>());
    if (!n)
        throw CommandError(std::string(cmd) + ": no texture set named '" + a.positional[0].get<std::string>() + "'");
    return *n;
}

/** A relative path lands where bake outputs do (the workspace, else the scene's folder). */
fs::path resolveFile(const std::string& file)
{
    fs::path p = paths::fromUtf8(file);
    if (p.is_absolute())
        return p;
    return bake::resolveFolder(paths::toUtf8(p.parent_path())) / p.filename();
}

/** A channel, or node.socket of the set's graph, as an output request. */
OutputRequest outputRequestFor(const Graph& g, const SetSettings& s, const std::string& source, const std::string& field)
{
    const auto dot = source.find('.');
    if (dot == std::string::npos)
    {
        for (const OutputRequest& r : channelRequests(g, s))
            if (r.name == source)
                return r;
        throw std::invalid_argument("no channel named " + source + " (or give node.socket)");
    }
    const std::string node = source.substr(0, dot), socket = source.substr(dot + 1);
    const GNode* n = g.node(node);
    if (!n)
        throw std::invalid_argument("no node named " + node);
    ChannelList extra = extraChannels(s);
    NodePorts p = portsOf(g, *n, &extra);
    const PortDecl* out = p.output(socket);
    if (!out)
        throw std::invalid_argument(node + " has no output " + socket);
    OutputRequest r{source, node, socket, out->type, {}};
    if (out->type == PortType::Material)
    {
        r.field = field.empty() ? "baseColor" : field;
        r.type = PortType::Color;
        for (const auto& [name, type] : standardChannels())
            if (name == r.field)
                r.type = type;
    }
    return r;
}

struct PsdLayer
{
    std::string name;
    std::string values;
    std::string mask;
    /** Folders the layer sits in, outermost first: their masks multiply in. */
    std::vector<std::string> folders;
    double opacity = 1.0;
};

void collectLayers(const LayerStack& stack, const std::vector<Layer>& list, const std::string& channel, std::vector<std::string> folders,
                   std::vector<PsdLayer>& out)
{
    for (const Layer& l : list)
    {
        if (!l.enabled)
            continue;
        if (l.kind == LayerKind::Folder)
        {
            std::vector<std::string> inner = folders;
            inner.push_back(l.id);
            collectLayers(stack, l.children, channel, inner, out);
            continue;
        }
        if (l.kind == LayerKind::Filter || !channelOn(stack, l, channel))
            continue;
        PsdLayer p;
        const std::string mode = channelMode(l, channel);
        p.name = (l.name.empty() ? l.id : l.name) + (mode == "normal" ? std::string() : " (" + mode + ")");
        p.values = l.id + "." + channel;
        p.mask = l.id + ".mask";
        p.folders = folders;
        if (auto it = l.channels.find(channel); it != l.channels.end())
            p.opacity = it->second.opacity;
        out.push_back(std::move(p));
    }
}

/** RGB from the values (gray broadcast), alpha from the masks. */
std::vector<float> layerPixels(const image::Image& values, const std::vector<const image::Image*>& masks, double opacity)
{
    std::vector<float> px(values.rgba.size());
    const size_t n = size_t(values.width) * size_t(values.height);
    for (size_t i = 0; i < n; ++i)
    {
        float a = float(opacity);
        for (const image::Image* m : masks)
            if (m && m->rgba.size() == values.rgba.size())
                a *= m->rgba[i * 4];
        px[i * 4 + 0] = values.rgba[i * 4 + 0];
        px[i * 4 + 1] = values.rgba[i * 4 + 1];
        px[i * 4 + 2] = values.rgba[i * 4 + 2];
        px[i * 4 + 3] = std::clamp(a, 0.0f, 1.0f);
    }
    return px;
}

bool opaque(const image::Image& img)
{
    for (size_t i = 3; i < img.rgba.size(); i += 4)
        if (img.rgba[i] < 0.999f)
            return false;
    return true;
}

/** Writes an imported image's colour and coverage as PNGs the layer reads. */
std::pair<fs::path, fs::path> splitImage(const image::Image& img, const fs::path& folder, const std::string& stem, bool alphaMask)
{
    image::Image rgb = img, mask = img;
    const size_t n = size_t(img.width) * size_t(img.height);
    for (size_t i = 0; i < n; ++i)
    {
        rgb.rgba[i * 4 + 3] = 1.0f;
        const float* p = &img.rgba[i * 4];
        const float v = alphaMask ? p[3] : 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        mask.rgba[i * 4 + 0] = mask.rgba[i * 4 + 1] = mask.rgba[i * 4 + 2] = v;
        mask.rgba[i * 4 + 3] = 1.0f;
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path rgbPath = folder / (stem + ".png"), maskPath = folder / (stem + "_mask.png");
    bake::prepareOutputs({rgbPath, maskPath});
    image::write(rgbPath, rgb, 8);
    image::write(maskPath, mask, 8);
    return {rgbPath, maskPath};
}
} // namespace

json exportLayerPsd(scene::Node& set, const PsdOptions& options)
{
    LILA_ZONE();
    const Graph g = readGraph(set);
    const SetSettings s = readSettings(set);
    const StackState state = readStack(g, s);
    const fs::path folder = bake::resolveFolder(options.folder.empty() ? "textures/" + set.name() : options.folder);
    std::error_code ec;
    fs::create_directories(folder, ec);

    json files = json::array(), warnings = json::array();
    std::vector<fs::path> written;
    const std::vector<OutputRequest> channels = channelRequests(g, s);
    for (const OutputRequest& ch : channels)
    {
        if (!options.channels.empty() && std::ranges::find(options.channels, ch.name) == options.channels.end())
            continue;
        std::vector<PsdLayer> layers;
        if (state.layered && !state.graphMode)
            collectLayers(state.stack, state.stack.layers, ch.name, {}, layers);
        else
            warnings.push_back(ch.name + ": the set isn't a layer stack; its composite is the only layer");

        std::vector<OutputRequest> requests;
        auto request = [&](const std::string& name, const std::string& node, const std::string& socket, PortType type) {
            if (std::ranges::none_of(requests, [&](const OutputRequest& r) { return r.name == name; }))
                requests.push_back({name, node, socket, type, {}});
        };
        if (layers.empty())
            requests.push_back(ch);
        for (const PsdLayer& l : layers)
        {
            request(l.values, l.values.substr(0, l.values.find('.')), ch.name, ch.type);
            request(l.mask, l.mask.substr(0, l.mask.find('.')), "mask", PortType::Gray);
            for (const std::string& f : l.folders)
                request(f + ".mask", f, "mask", PortType::Gray);
        }

        const Channel* c = s.channel(ch.name);
        const int bits = c && c->bits >= 16 ? 16 : 8;
        for (const Tile& t : s.tiles)
        {
            std::vector<std::string> w;
            const auto out = evaluateOutputs(set, requests, t.udim, options.width, options.height, options.backend, &w);
            for (const std::string& x : w)
                warnings.push_back(x);
            const image::Image& first = out.begin()->second;
            const int width = first.width, height = first.height;

            std::vector<std::pair<std::string, std::vector<float>>> pixels;
            if (layers.empty())
                pixels.emplace_back(ch.name, out.at(ch.name).rgba);
            // writePsd wants the top layer first.
            for (auto it = layers.rbegin(); it != layers.rend(); ++it)
            {
                std::vector<const image::Image*> masks{&out.at(it->mask)};
                for (const std::string& f : it->folders)
                    masks.push_back(&out.at(f + ".mask"));
                pixels.emplace_back(it->name, layerPixels(out.at(it->values), masks, it->opacity));
            }
            if (!layers.empty())
            {
                const json base = state.stack.base.contains(ch.name) ? state.stack.base[ch.name] : channelDefault(ch.name, ch.type);
                float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                valueToFloat4(base, ch.type == PortType::Material ? PortType::Color : ch.type, v);
                std::vector<float> solid(size_t(width) * size_t(height) * 4);
                for (size_t i = 0; i < solid.size(); i += 4)
                {
                    solid[i] = v[0];
                    solid[i + 1] = ch.type == PortType::Gray || ch.type == PortType::Float ? v[0] : v[1];
                    solid[i + 2] = ch.type == PortType::Gray || ch.type == PortType::Float ? v[0] : v[2];
                    solid[i + 3] = 1.0f;
                }
                pixels.emplace_back("Base", std::move(solid));
            }
            std::vector<image::Layer> psd;
            for (const auto& [name, px] : pixels)
                psd.push_back({name, px});
            const fs::path path =
                folder / (bake::fileSafe(set.name()) + "_" + ch.name + (s.tiles.size() > 1 ? "." + std::to_string(t.udim) : std::string()) + ".psd");
            bake::prepareOutputs({path});
            image::writePsd(path, width, height, psd, bits);
            written.push_back(path);
            files.push_back(paths::toUtf8(path));
        }
    }
    bake::addOutputs(written);
    return {{"files", files}, {"warnings", warnings}};
}

namespace
{
bool under(const fs::path& file, const fs::path& dir)
{
    if (dir.empty())
        return false;
    const fs::path rel = file.lexically_normal().lexically_relative(dir.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

void imagesOf(const Graph& g, std::vector<fs::path>& out)
{
    for (const GNode& n : g.nodes)
    {
        NodePorts p = portsOf(g, n);
        for (const ParamDecl& d : p.params)
            if (d.type == ParamType::Image)
                if (const json v = paramValue(n, d); v.is_string() && !v.get<std::string>().empty() && !v.get<std::string>().starts_with("external:"))
                    out.push_back(resolveImagePath(v.get<std::string>(), {}));
    }
    for (const auto& [name, sub] : g.subgroups)
    {
        try
        {
            imagesOf(Graph::fromJson(sub), out);
        }
        catch (const std::exception&)
        {
        }
    }
}
} // namespace

std::vector<fs::path> sceneTextureFiles(scene::Graph& g, const fs::path& scene)
{
    std::vector<fs::path> found;
    for (const std::string& name : setNames(g))
        if (scene::Node* n = findSet(g, name))
        {
            try
            {
                imagesOf(readGraph(*n), found);
            }
            catch (const std::exception&)
            {
            }
        }
    const fs::path root = Workspace::get().isOpen() ? Workspace::get().root() : fs::path();
    const fs::path sceneDir = scene.parent_path();
    std::vector<fs::path> out;
    std::error_code ec;
    for (const fs::path& f : found)
        if ((under(f, root) || under(f, sceneDir)) && fs::is_regular_file(f, ec) && std::ranges::find(out, f) == out.end())
            out.push_back(f);
    fs::path store = scene;
    store += ".lilatex";
    if (fs::is_directory(store, ec))
        for (const auto& e : fs::recursive_directory_iterator(store, ec))
            if (e.is_regular_file(ec))
                out.push_back(e.path());
    return out;
}

void onSceneSaved(const fs::path& scene)
{
    LILA_ZONE();
    bake::addOutputs(sceneTextureFiles(scene::Graph::current(), scene));
}

/** Texture sets as textureDeformer's source: evaluated on the CPU, cached until the set's graph, settings or paint change. */
static void installDeformerSource()
{
    scene::deformers::setTextureSetSource([](const std::string& name, const std::string& source, bool refresh) -> std::shared_ptr<const image::Image> {
        LILA_ZONE_N("tex::deformerSource");

        static std::mutex mutex;
        static std::map<std::string, std::pair<std::string, std::shared_ptr<const image::Image>>> cache;
        scene::Node* set = findSet(scene::Graph::current(), name);
        if (!set)
            return nullptr;
        const std::string stamp = set->as<std::string>(set->type().attrIndex("graph")) + '\n' + set->as<std::string>(set->type().attrIndex("settings")) +
                                  '\n' + std::to_string(reinterpret_cast<uintptr_t>(set->as<scene::DataPtr>(set->type().attrIndex(paint::kPaintAttr)).get()));
        std::lock_guard lock(mutex);
        auto& slot = cache[name + '\n' + source];
        if (!refresh && slot.second && slot.first == stamp)
            return slot.second;
        const Graph g = readGraph(*set);
        const SetSettings s = readSettings(*set);
        const OutputRequest r = outputRequestFor(g, s, source, "");
        auto out = evaluateOutputs(*set, {r}, s.tiles.front().udim, 0, 0, "cpu");
        slot = {stamp, std::make_shared<image::Image>(std::move(out.at(r.name)))};
        return slot.second;
    });
}

void registerInteropCommands()
{
    using T = ArgType;
    installDeformerSource();
    ops::registerCommand({
        .name = "texExportImage",
        .doc = "Write one channel or any node output (node.socket) of a texture set as an image, for one tile. A material socket takes "
               "field= (baseColor by default). Returns {file, width, height, warnings}.",
        .positionalDoc = "set source file",
        .flags = {presetFlag({"resolution", "", T::Int, nullptr, "Width and height; the tile's when left out.", {}, {}, 1.0}, kTextureSizePresets),
                  {"udim", "", T::Int, nullptr, "The tile; the first when left out.", {}, {}, 1001.0},
                  {"bits", "", T::Int, 0, "8, 16 or 32 (exr, tif); 0 uses the channel's, else 8.", {}, {}, 0.0, 32.0},
                  {"field", "", T::String, nullptr, "Material sockets: the channel taken."},
                  {"backend", "", T::String, nullptr, "gpu or cpu.", {"gpu", "cpu"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Node& set = setArg(a, "texExportImage");
            if (a.positional.size() < 3 || !a.positional[1].is_string() || !a.positional[2].is_string())
                throw CommandError("texExportImage: give the set, a channel or node.socket, and the file");
            const std::string source = a.positional[1].get<std::string>();
            const fs::path file = resolveFile(a.positional[2].get<std::string>());
            try
            {
                const Graph g = readGraph(set);
                const SetSettings s = readSettings(set);
                const OutputRequest r = outputRequestFor(g, s, source, a.get<std::string>("field", ""));
                const int res = a.get<int>("resolution", 0);
                std::vector<std::string> warnings;
                const auto out = evaluateOutputs(set, {r}, a.get<int>("udim", s.tiles.front().udim), res, res, a.get<std::string>("backend", ""), &warnings);
                const image::Image& img = out.at(r.name);
                int bits = a.get<int>("bits", 0);
                if (bits <= 0)
                    bits = s.channel(source) ? s.channel(source)->bits : 8;
                const std::string ext = file.extension().string();
                if (ext == ".png" || ext == ".psd")
                    bits = std::min(bits, 16);
                std::error_code ec;
                fs::create_directories(file.parent_path(), ec);
                bake::prepareOutputs({file});
                image::WriteOptions wo;
                wo.bits = bits;
                image::write(file, img, wo);
                bake::addOutputs({file});
                return {{"file", paths::toUtf8(file)}, {"width", img.width}, {"height", img.height}, {"warnings", warnings}};
            }
            catch (const CommandError&)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texExportImage: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "texExportPsd",
        .doc = "Export a texture set's layer stack as one layered PSD per channel (and tile): a Base layer, then each layer writing the "
               "channel with its values in RGB and its mask times opacity in alpha. Returns {files, warnings}.",
        .positionalDoc = "set",
        .flags = {{"channels", "", T::List, nullptr, "Channel names; all when left out."},
                  {"folder", "", T::String, nullptr, "Output folder; relative ones resolve like bake folders. Default textures/<set>."},
                  presetFlag({"resolution", "", T::Int, nullptr, "Width and height, overriding the tiles'.", {}, {}, 1.0}, kTextureSizePresets),
                  {"backend", "", T::String, nullptr, "gpu or cpu.", {"gpu", "cpu"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Node& set = setArg(a, "texExportPsd");
            PsdOptions o;
            if (a.has("channels"))
                o.channels = a.flags["channels"].get<std::vector<std::string>>();
            o.folder = a.get<std::string>("folder", "");
            o.width = o.height = a.get<int>("resolution", 0);
            o.backend = a.get<std::string>("backend", "");
            try
            {
                return exportLayerPsd(set, o);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texExportPsd: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "texImportImage",
        .doc = "Bring images into a texture set's stack as layers, bottom to top: each image (a PSD's layers each, unless layers=False) "
               "becomes a paint layer whose channel image is its colour and whose painted mask is its alpha, or with kind=fill a fill "
               "layer (material=) masked by the image's alpha (its luminance when it is opaque). The pixels are copied to "
               "textures/<set>/imported. Returns {layers, files}.",
        .positionalDoc = "set file [file...]",
        .flags = {{"kind", "", T::String, "paint", "paint or fill.", {"paint", "fill"}},
                  {"channel", "", T::String, "baseColor", "Paint: the channel the colour goes to."},
                  {"material", "", T::Any, nullptr, "Fill: channel values."},
                  {"layers", "", T::Bool, true, "A PSD's layers as separate layers (else its flattened image)."},
                  {"parent", "", T::String, nullptr, "Folder to put them in."},
                  {"folder", "", T::String, nullptr, "Where the copies go; textures/<set>/imported by default."}},
        .run = [](const Args& a) -> json {
            scene::Node& set = setArg(a, "texImportImage");
            if (a.positional.size() < 2)
                throw CommandError("texImportImage: give at least one image");
            const std::string kind = a.get<std::string>("kind", "paint");
            const std::string channel = a.get<std::string>("channel", "baseColor");
            if (kind == "paint" && !readSettings(set).channel(channel))
                throw CommandError("texImportImage: the set has no channel " + channel);
            const fs::path folder = bake::resolveFolder(a.get<std::string>("folder", "textures/" + set.name() + "/imported"));

            std::vector<std::pair<std::string, image::Image>> images;
            for (size_t i = 1; i < a.positional.size(); ++i)
            {
                if (!a.positional[i].is_string())
                    throw CommandError("texImportImage: files are strings");
                const fs::path file = resolveFile(a.positional[i].get<std::string>());
                std::string err;
                std::string ext = file.extension().string();
                std::ranges::transform(ext, ext.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
                if (ext == ".psd" && a.get<bool>("layers", true))
                {
                    auto layers = image::readPsdLayers(file, &err);
                    if (layers.empty())
                        throw CommandError("texImportImage: can't read the layers of " + paths::toUtf8(file) + (err.empty() ? "" : ": " + err));
                    for (auto& [name, img] : layers)
                        images.emplace_back(paths::toUtf8(file.stem()) + "_" + name, std::move(img));
                    continue;
                }
                image::Image img = image::read(file, &err);
                if (img.width <= 0)
                    throw CommandError("texImportImage: can't read " + paths::toUtf8(file) + (err.empty() ? "" : ": " + err));
                images.emplace_back(paths::toUtf8(file.stem()), std::move(img));
            }

            UndoChunk chunk("Import Image Layers");
            json ids = json::array(), files = json::array();
            std::vector<fs::path> written;
            const std::string setName = set.name();
            for (const auto& [name, img] : images)
            {
                const bool alpha = !opaque(img);
                const std::string stem = bake::fileSafe(name) + "_" + (kind == "paint" ? channel : std::string("fill"));
                const auto [rgbPath, maskPath] = splitImage(img, folder, stem, kind == "paint" || alpha);
                written.push_back(rgbPath);
                written.push_back(maskPath);
                json add = {{"args", {setName, kind}}, {"name", name}};
                if (a.has("parent"))
                    add["parent"] = a.flags["parent"];
                if (kind == "fill" && a.has("material"))
                    add["material"] = a.flags["material"];
                // Only the imported channel: a paint layer writes every channel its material holds.
                if (kind == "paint")
                    add["material"] = json::object();
                const std::string id = ops::execute("texAddLayer", add).at("id").get<std::string>();
                if (kind == "paint")
                    ops::execute("texSetLayer", {{"args", {setName, id, {{"paint", {{channel, paths::toUtf8(rgbPath)}, {"mask", paths::toUtf8(maskPath)}}}}}}});
                else
                    ops::execute("texAddMask", {{"args", {setName, id, "paint"}}, {"image", paths::toUtf8(maskPath)}});
                ids.push_back(id);
                files.push_back(paths::toUtf8(rgbPath));
                files.push_back(paths::toUtf8(maskPath));
            }
            bake::addOutputs(written);
            return {{"layers", ids}, {"files", files}};
        },
    });
}
} // namespace lila::tex
