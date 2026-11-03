#include "scene/TextureDeformers.h"

#include "ops/Command.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "deform/Surface.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "scene/Anim.h"
#include "scene/DeformerUtil.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/RigUtil.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>

namespace lila::scene::deformers
{
using namespace rigutil;
using namespace deformutil;

namespace
{
std::mutex gSourceMutex;
TextureSetSource gSetSource;

double componentOf(const glm::vec4& c, int64_t component)
{
    switch (component)
    {
    case 1:
        return c.r;
    case 2:
        return c.g;
    case 3:
        return c.b;
    case 4:
        return c.a;
    default:
        return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    }
}

/** Displaces points by an image read through a uv set: along the normal by one component, or by its rgb as a vector. */
class TextureDeformerNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        // Fresh from the menu it has no image yet: leave the mesh alone rather than report an error every evaluation.
        if (as<std::string>(attr("image")).empty() && as<std::string>(attr("textureSet")).empty())
            return;
        std::shared_ptr<const image::Image> img = texture();
        const std::string& set = as<std::string>(attr("uvSet"));
        std::span<const glm::vec2> uvs = in.cornerAttrs().get<glm::vec2>(set);
        if (uvs.empty())
            throw std::runtime_error("the mesh has no uv set " + set);
        // Each point averages its corners' samples, so a point on a uv seam takes both sides.
        std::vector<glm::dvec4> sum(points.size(), glm::dvec4(0.0));
        std::vector<int> count(points.size(), 0);
        for (geo::Index c = 0; c < in.cornerCapacity() && c < uvs.size(); ++c)
        {
            if (!in.cornerValid(c))
                continue;
            geo::Index p = in.cornerPoint(c);
            if (p >= points.size())
                continue;
            sum[p] += glm::dvec4(sampleUv(*img, uvs[c]));
            ++count[p];
        }
        const double strength = num("strength"), mid = num("midLevel");
        const bool alongNormal = integer("direction") == 0;
        const int64_t component = integer("component");
        std::vector<glm::dvec3> normals;
        if (alongNormal)
            normals = deform::pointNormals(in, points);
        for (size_t p = 0; p < points.size(); ++p)
        {
            if (count[p] == 0)
                continue;
            const glm::dvec4 v = sum[p] / double(count[p]);
            if (alongNormal)
                points[p] += normals[p] * ((componentOf(glm::vec4(v), component) - mid) * strength);
            else
                points[p] += (glm::dvec3(v) - mid) * strength;
        }
    }

private:
    std::shared_ptr<const image::Image> texture() const
    {
        const std::string& file = as<std::string>(attr("image"));
        const std::string& texSet = as<std::string>(attr("textureSet"));
        const std::string& source = as<std::string>(attr("source"));
        const int64_t reload = integer("reload");
        std::lock_guard lock(mMutex);
        if (!texSet.empty())
        {
            const std::string key = texSet + "\n" + source;
            if (!mImage || key != mKey || reload != mReload)
            {
                TextureSetSource fn;
                {
                    std::lock_guard g(gSourceMutex);
                    fn = gSetSource;
                }
                if (!fn)
                    throw std::runtime_error("texture sets are not available");
                mImage = fn(texSet, source, reload != mReload);
                if (!mImage)
                    throw std::runtime_error("no texture set " + texSet);
                mKey = key;
                mReload = reload;
            }
            return mImage;
        }
        if (file.empty())
            throw std::runtime_error("no image: set image or textureSet");
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(paths::fromUtf8(file), ec);
        if (!mImage || file != mKey || stamp != mStamp || reload != mReload)
        {
            std::string err;
            auto img = std::make_shared<image::Image>(image::read(paths::fromUtf8(file), &err));
            if (img->width <= 0 || img->height <= 0)
                throw std::runtime_error(err.empty() ? "can't read " + file : err);
            mImage = std::move(img);
            mKey = file;
            mStamp = stamp;
            mReload = reload;
        }
        return mImage;
    }

    mutable std::mutex mMutex;
    mutable std::shared_ptr<const image::Image> mImage;
    mutable std::string mKey;
    mutable std::filesystem::file_time_type mStamp{};
    mutable int64_t mReload = -1;
};

int64_t choiceIndex(const std::string& value, std::initializer_list<const char*> names, const char* cmd, const char* flag)
{
    int64_t i = 0;
    std::string all;
    for (const char* n : names)
    {
        if (value == n)
            return i;
        all += (all.empty() ? "" : ", ") + std::string(n);
        ++i;
    }
    throw CommandError(std::format("{}: {} must be one of {}", cmd, flag, all));
}

/** A tension deformer by name, or the topmost one in a mesh's stack when given the mesh. */
DeformerNode& tensionNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (n.type().name == "tension")
        return static_cast<DeformerNode&>(n);
    if (n.type().isA("mesh"))
    {
        std::vector<NodeId> stack = list(Graph::current(), n.id());
        for (auto it = stack.rbegin(); it != stack.rend(); ++it)
            if (Graph::current().node(*it)->type().name == "tension")
                return static_cast<DeformerNode&>(*Graph::current().node(*it));
    }
    throw CommandError(std::string(cmd) + ": " + name + " is not a tension deformer, nor a mesh with one");
}

/** Per point: the most (or mean) stretch in red and compression in green over the frames, 0 to 1 at the deformer's scale. */
std::vector<glm::vec4> sampleTension(Graph& graph, DeformerNode& d, const std::vector<double>& frames, bool mean)
{
    std::vector<glm::vec4> out;
    const double saved = graph.time();
    for (double f : frames)
    {
        graph.setTime(anim::secondsOf(f));
        graph.evaluate();
        MeshPtr m = std::get<MeshPtr>(graph.value({d.id(), d.outAttr, -1}));
        std::span<const float> t = m ? m->pointAttrs().get<float>("tension") : std::span<const float>();
        if (out.empty())
            out.assign(t.size(), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        const double scale = d.num("scale");
        for (size_t p = 0; p < std::min(t.size(), out.size()); ++p)
        {
            const float v = float(std::clamp(double(t[p]) * scale, -1.0, 1.0));
            if (mean)
            {
                out[p].r += std::max(v, 0.0f) / float(frames.size());
                out[p].g += std::max(-v, 0.0f) / float(frames.size());
            }
            else
            {
                out[p].r = std::max(out[p].r, v);
                out[p].g = std::max(out[p].g, -v);
            }
        }
    }
    graph.setTime(saved);
    graph.evaluate();
    return out;
}
} // namespace

void setTextureSetSource(TextureSetSource fn)
{
    std::lock_guard lock(gSourceMutex);
    gSetSource = std::move(fn);
}

glm::vec4 sampleUv(const image::Image& img, const glm::dvec2& uv)
{
    if (img.width <= 0 || img.height <= 0 || img.rgba.size() < size_t(img.width) * size_t(img.height) * 4)
        return glm::vec4(0.0f);
    const double x = uv.x * img.width - 0.5, y = (1.0 - uv.y) * img.height - 0.5;
    const double fx = std::floor(x), fy = std::floor(y);
    const float tx = float(x - fx), ty = float(y - fy);
    auto wrap = [](int64_t i, int n) { return int(((i % n) + n) % n); };
    auto at = [&](int64_t ix, int64_t iy) {
        const size_t i = (size_t(wrap(iy, img.height)) * size_t(img.width) + size_t(wrap(ix, img.width))) * 4;
        return glm::vec4(img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]);
    };
    const int64_t ix = int64_t(fx), iy = int64_t(fy);
    const glm::vec4 top = glm::mix(at(ix, iy), at(ix + 1, iy), tx);
    const glm::vec4 bottom = glm::mix(at(ix, iy + 1), at(ix + 1, iy + 1), tx);
    return glm::mix(top, bottom, ty);
}

image::Image rasterizeByUv(const geo::Mesh& mesh, std::string_view uvSet, std::span<const glm::vec4> pointColours, int width, int height, int padding)
{
    LILA_ZONE();

    image::Image img;
    std::span<const glm::vec2> uvs = mesh.cornerAttrs().get<glm::vec2>(uvSet);
    if (uvs.empty() || width <= 0 || height <= 0)
        return img;
    img.width = width;
    img.height = height;
    img.rgba.assign(size_t(width) * size_t(height) * 4, 0.0f);
    std::vector<char> covered(size_t(width) * size_t(height), 0);
    auto colourOf = [&](geo::Index c) {
        geo::Index p = mesh.cornerPoint(c);
        return p < pointColours.size() ? pointColours[p] : glm::vec4(0.0f);
    };
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        std::vector<geo::Index> corners = mesh.faceCorners(f);
        for (size_t k = 1; k + 1 < corners.size(); ++k)
        {
            const geo::Index c[3] = {corners[0], corners[k], corners[k + 1]};
            if (c[0] >= uvs.size() || c[1] >= uvs.size() || c[2] >= uvs.size())
                continue;
            // Texel space, y down: row 0 is v = 1.
            glm::dvec2 q[3];
            for (int i = 0; i < 3; ++i)
                q[i] = {uvs[c[i]].x * width, (1.0 - uvs[c[i]].y) * height};
            const double area = (q[1].x - q[0].x) * (q[2].y - q[0].y) - (q[2].x - q[0].x) * (q[1].y - q[0].y);
            if (std::abs(area) < 1e-12)
                continue;
            const int x0 = std::max(0, int(std::floor(std::min({q[0].x, q[1].x, q[2].x}))));
            const int x1 = std::min(width - 1, int(std::ceil(std::max({q[0].x, q[1].x, q[2].x}))));
            const int y0 = std::max(0, int(std::floor(std::min({q[0].y, q[1].y, q[2].y}))));
            const int y1 = std::min(height - 1, int(std::ceil(std::max({q[0].y, q[1].y, q[2].y}))));
            const glm::vec4 col[3] = {colourOf(c[0]), colourOf(c[1]), colourOf(c[2])};
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const glm::dvec2 s(x + 0.5, y + 0.5);
                    const double w0 = ((q[1].x - s.x) * (q[2].y - s.y) - (q[2].x - s.x) * (q[1].y - s.y)) / area;
                    const double w1 = ((q[2].x - s.x) * (q[0].y - s.y) - (q[0].x - s.x) * (q[2].y - s.y)) / area;
                    const double w2 = 1.0 - w0 - w1;
                    if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9)
                        continue;
                    const glm::vec4 v = col[0] * float(w0) + col[1] * float(w1) + col[2] * float(w2);
                    const size_t i = size_t(y) * size_t(width) + size_t(x);
                    std::copy(&v[0], &v[0] + 4, img.rgba.begin() + std::ptrdiff_t(i * 4));
                    covered[i] = 1;
                }
        }
    }
    for (int pass = 0; pass < padding; ++pass)
    {
        std::vector<char> next = covered;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const size_t i = size_t(y) * size_t(width) + size_t(x);
                if (covered[i])
                    continue;
                glm::vec4 sum(0.0f);
                int n = 0;
                for (auto [dx, dy] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}})
                {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                        continue;
                    const size_t j = size_t(ny) * size_t(width) + size_t(nx);
                    if (!covered[j])
                        continue;
                    sum += glm::vec4(img.rgba[j * 4], img.rgba[j * 4 + 1], img.rgba[j * 4 + 2], img.rgba[j * 4 + 3]);
                    ++n;
                }
                if (n == 0)
                    continue;
                sum /= float(n);
                std::copy(&sum[0], &sum[0] + 4, img.rgba.begin() + std::ptrdiff_t(i * 4));
                next[i] = 1;
            }
        covered = std::move(next);
    }
    return img;
}

bool rebindTexture(Graph& graph, DeformerNode& d)
{
    if (d.type().name != "textureDeformer")
        return false;
    graph.setValue({d.id(), d.attr("reload"), -1}, d.integer("reload") + 1);
    return true;
}

void registerTextureTypes()
{
    registerDeformer({"textureDeformer", "photo",
                      "Displaces the mesh by an image read through a uv set: along the normal by one component around midLevel, "
                      "or by its rgb as an object-space vector. The image comes from a file or a texture set's channel.",
                      {stored("image", PlugType::String, std::string(), "Image file (png, exr, tga, tif, psd). Used when textureSet is empty."),
                       stored("textureSet", PlugType::String, std::string(), "Texture set whose output displaces; empty uses image."),
                       stored("source", PlugType::String, std::string("height"), "The texture set's channel, or node.socket."),
                       stored("uvSet", PlugType::String, std::string("uv0"), "The uv set the image is read through."),
                       keyable("strength", PlugType::Float, 1.0, "Displacement per unit of image value, object units."),
                       keyable("midLevel", PlugType::Float, 0.5, "The image value that doesn't move a point."),
                       enumAttr("direction", 0, {"normal", "vector"}, "Along each point's normal, or the rgb as an xyz offset."),
                       enumAttr("component", 0, {"luminance", "red", "green", "blue", "alpha"}, "What the normal direction reads."),
                       {.name = "reload", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable | AttrHidden,
                        .doc = "Bumped by rebindDeformer to read the image or texture set again."}},
                      [] { return std::make_unique<TextureDeformerNode>(); }});
}

void registerTextureCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "textureDeformer",
        .doc = "Add a texture deformer: displaces the meshes by an image file or a texture set's channel, read through a uv set. "
               "rebindDeformer reads the image again. Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"image", "", T::String, nullptr, "Image file."},
                  {"textureSet", "", T::String, nullptr, "Texture set to read instead of a file."},
                  {"source", "", T::String, "height", "The texture set's channel, or node.socket."},
                  {"uvSet", "", T::String, "uv0", "The uv set read."},
                  {"strength", "", T::Float, 1.0, "Displacement per unit of image value."},
                  {"midLevel", "", T::Float, 0.5, "The image value that doesn't move a point."},
                  {"direction", "", T::String, "normal", "normal | vector", {"normal", "vector"}},
                  {"component", "", T::String, "luminance", "What normal reads.", {"luminance", "red", "green", "blue", "alpha"}},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "textureDeformer";
            Graph& graph = Graph::current();
            std::vector<NodeId> meshes;
            for (const std::string& o : a.objects())
                meshes.push_back(meshNamed(o, cmd).id());
            if (meshes.empty())
                throw CommandError("textureDeformer: give or select at least one mesh");
            const int64_t direction = choiceIndex(a.get<std::string>("direction"), {"normal", "vector"}, cmd, "direction");
            const int64_t component = choiceIndex(a.get<std::string>("component"), {"luminance", "red", "green", "blue", "alpha"}, cmd, "component");
            json out = json::array();
            for (NodeId id : addDeformer(graph, "textureDeformer", meshes, a.get<std::string>("name", "TextureDeformer")))
            {
                setPlug(graph, id, "image", a.get<std::string>("image", ""));
                setPlug(graph, id, "textureSet", a.get<std::string>("textureSet", ""));
                setPlug(graph, id, "source", a.get<std::string>("source"));
                setPlug(graph, id, "uvSet", a.get<std::string>("uvSet"));
                setPlug(graph, id, "strength", a.get<double>("strength"));
                setPlug(graph, id, "midLevel", a.get<double>("midLevel"));
                setPlug(graph, id, "direction", direction);
                setPlug(graph, id, "component", component);
                out.push_back(graph.node(id)->name());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "bakeTension",
        .doc = "Bake a tension deformer over a frame range into a mask Unreal can read: red the most stretch, green the most "
               "compression (0 to 1 at the deformer's scale), as a colour set on the mesh's base shape and/or an image by uv. "
               "Returns {frames, colorSet, file, maxStretch, maxCompress}.",
        .positionalDoc = "tension deformer or mesh",
        .flags = {{"start", "", T::Int, nullptr, "First frame; the playback start by default."},
                  {"end", "", T::Int, nullptr, "Last frame; the playback end by default."},
                  {"step", "", T::Int, 1, "Frames between samples.", {}, {}, 1.0},
                  {"mode", "", T::String, "max", "max keeps each point's peak, mean its average.", {"max", "mean"}},
                  {"colorSet", "", T::String, "tensionMask", "Colour set written on the base mesh; empty for none."},
                  {"file", "f", T::String, nullptr, "Image to write by uv (png, exr, tga, tif)."},
                  {"resolution", "", T::Int, 1024, "Image width and height.", {}, {}, 1.0, 16384.0},
                  {"uvSet", "", T::String, "uv0", "The uv set the image is laid out by."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "bakeTension";
            Graph& graph = Graph::current();
            std::vector<std::string> objs = a.objects();
            if (objs.size() != 1)
                throw CommandError("bakeTension: give one tension deformer or a mesh with one");
            DeformerNode& d = tensionNamed(objs.front(), cmd);
            const NodeId mesh = modifiers::meshOf(graph, d.id());
            if (mesh == kNoNode)
                throw CommandError("bakeTension: " + d.name() + " is not on a mesh");
            const anim::Playback& pb = anim::playback();
            const int start = a.has("start") ? a.get<int>("start") : int(std::lround(anim::frameOf(pb.minTime)));
            const int end = a.has("end") ? a.get<int>("end") : int(std::lround(anim::frameOf(pb.maxTime)));
            const int step = std::max(1, a.get<int>("step"));
            if (end < start)
                throw CommandError("bakeTension: end is before start");
            const std::string mode = a.get<std::string>("mode");
            if (mode != "max" && mode != "mean")
                throw CommandError("bakeTension: mode must be max or mean");
            std::vector<double> frames;
            for (int f = start; f <= end; f += step)
                frames.push_back(double(f));
            std::vector<glm::vec4> mask = sampleTension(graph, d, frames, mode == "mean");
            if (mask.empty())
                throw CommandError("bakeTension: " + d.name() + " wrote no tension; is it bound and enabled?");

            const PlugRef plug = modifiers::editPlug(graph, mesh);
            MeshPtr base = std::get<MeshPtr>(graph.value(plug));
            if (!base || base->pointCapacity() != mask.size())
                throw CommandError("bakeTension: the stack changes the point count, so the mask can't go on the base mesh");
            json out = {{"frames", frames.size()}, {"colorSet", nullptr}, {"file", nullptr}};
            float maxStretch = 0.0f, maxCompress = 0.0f;
            for (const glm::vec4& c : mask)
            {
                maxStretch = std::max(maxStretch, c.r);
                maxCompress = std::max(maxCompress, c.g);
            }
            out["maxStretch"] = maxStretch;
            out["maxCompress"] = maxCompress;
            const std::string set = a.get<std::string>("colorSet");
            if (!set.empty())
            {
                auto m = std::make_shared<geo::Mesh>(*base);
                geo::colors::add(*m, set, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                geo::colors::setFromPoints(*m, set, mask);
                guarded([&] {
                    graph.setValue(plug, MeshPtr(m));
                    return 0;
                });
                out["colorSet"] = set;
            }
            if (a.has("file"))
            {
                const int res = std::clamp(a.get<int>("resolution"), 1, 16384);
                image::Image img = rasterizeByUv(*base, a.get<std::string>("uvSet"), mask, res, res);
                if (img.rgba.empty())
                    throw CommandError(std::format("bakeTension: {} has no uv set {}", graph.node(mesh)->name(), a.get<std::string>("uvSet")));
                const std::string file = a.get<std::string>("file");
                std::error_code ec;
                std::filesystem::create_directories(paths::fromUtf8(file).parent_path(), ec);
                try
                {
                    image::write(paths::fromUtf8(file), img, 16);
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("bakeTension: ") + e.what());
                }
                out["file"] = file;
            }
            return out;
        },
    });
}
} // namespace lila::scene::deformers
