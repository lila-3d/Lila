#include "scene/ImagePlane.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "core/Paths.h"
#include "scene/Graph.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

namespace lila::scene
{
namespace
{
using T = ArgType;
namespace ip = imagePlane;

constexpr uint32_t kStored = AttrInput | AttrStorable;
const std::vector<std::string> kFitNames = {"fill", "best", "horizontal", "vertical", "toSize"};
const std::vector<std::string> kDisplayModeNames = {"lookingThrough", "allViews"};
const std::vector<std::string> kDisplayColorNames = {"color", "grayscale"};
const std::vector<std::string> kPlacementNames = {"behind", "inScene", "inFront"};
const char* const kViews[] = {"persp", "top", "bottom", "front", "back", "left", "right"};

AttrDef floatAttr(std::string name, double def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Float, .defaultValue = def, .flags = kStored | AttrKeyable, .doc = std::move(doc)};
}

AttrDef boolAttr(std::string name, bool def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Bool, .defaultValue = def, .flags = kStored, .doc = std::move(doc)};
}

AttrDef enumAttr(std::string name, std::vector<std::string> names, int64_t def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Enum, .defaultValue = def, .flags = kStored, .enumNames = std::move(names),
            .doc = std::move(doc)};
}

std::vector<AttrDef> imagePlaneAttrs()
{
    std::vector<AttrDef> a(ImagePlane::kCount - Transform::kCount);
    auto at = [&](int attr) -> AttrDef& { return a[size_t(attr - Transform::kCount)]; };
    at(ImagePlane::kImageName) = {.name = "imageName", .type = PlugType::String, .defaultValue = std::string(), .flags = kStored | AttrFile,
                                  .doc = "Image file (PNG, JPG, TGA, HDR, BMP). Relative paths resolve from the workspace root, then the scene folder."};
    at(ImagePlane::kUseFrameExtension) =
        boolAttr("useFrameExtension", false, "Image sequence: the file name's frame number follows the time slider.");
    at(ImagePlane::kFrameOffset) = {.name = "frameOffset", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = kStored,
                                    .doc = "Added to the current frame to pick the sequence file."};
    at(ImagePlane::kCamera) = {.name = "camera", .type = PlugType::String, .defaultValue = std::string(), .flags = kStored,
                               .doc = "View the plane follows (persp, front, side, top, ...). Empty: free in the world, placed by its transform."};
    at(ImagePlane::kDisplayMode) =
        enumAttr("displayMode", kDisplayModeNames, 0, "Attached planes: only when looking through the camera, or in every view.");
    at(ImagePlane::kFit) = enumAttr("fit", kFitNames, 1,
                                    "How the image fills its frame (the film gate on a perspective camera, else width x height): fill crops, "
                                    "best fits inside, horizontal and vertical match one side, toSize stretches.");
    at(ImagePlane::kWidth) = floatAttr("width", 100.0, "Frame width, cm.");
    at(ImagePlane::kHeight) = floatAttr("height", 100.0, "Frame height, cm. Ignored while maintainRatio is on.");
    at(ImagePlane::kMaintainRatio) = boolAttr("maintainRatio", true, "Height follows the width at the image's aspect.");
    at(ImagePlane::kOffsetX) = floatAttr("offsetX", 0.0, "Moves the image right within its plane, cm.");
    at(ImagePlane::kOffsetY) = floatAttr("offsetY", 0.0, "Moves the image up within its plane, cm.");
    at(ImagePlane::kDepth) = floatAttr("depth", 1000.0,
                                       "Attached planes: distance in front of a perspective eye, or behind the origin along an orthographic view, cm.");
    at(ImagePlane::kRotation) = floatAttr("rotation", 0.0, "Turns the image around the plane's normal, degrees.");
    at(ImagePlane::kAlphaGain) = floatAttr("alphaGain", 1.0, "Opacity, 0 to 1, times the image's own alpha.");
    at(ImagePlane::kBrightness) = floatAttr("brightness", 0.0, "Added to the displayed color, -1 to 1.");
    at(ImagePlane::kContrast) = floatAttr("contrast", 1.0, "Scales the displayed color around mid gray; 1 leaves it alone.");
    at(ImagePlane::kDisplayColor) = enumAttr("displayColor", kDisplayColorNames, 0, "Show the image in color or as grayscale.");
    at(ImagePlane::kPlacement) =
        enumAttr("placement", kPlacementNames, 0, "Draw behind all geometry, depth tested in the scene, or in front of everything.");
    at(ImagePlane::kLocked) = boolAttr("locked", false, "Reference mode: the plane can't be picked in the viewport, only from the outliner.");
    return a;
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/** Index of an enum flag given by name (case-insensitive) or number. */
int64_t enumValue(const std::vector<std::string>& names, const json& v, const std::string& flag)
{
    if (v.is_number_integer() && v.get<int64_t>() >= 0 && size_t(v.get<int64_t>()) < names.size())
        return v.get<int64_t>();
    if (v.is_string())
        for (size_t i = 0; i < names.size(); ++i)
            if (lower(names[i]) == lower(v.get<std::string>()))
                return int64_t(i);
    std::string all;
    for (const std::string& n : names)
        all += (all.empty() ? "" : ", ") + n;
    throw CommandError("imagePlane: " + flag + " is one of " + all);
}

glm::dvec3 rotateInPlane(const glm::dvec3& u, const glm::dvec3& v, double degrees, bool second)
{
    double r = glm::radians(degrees), c = std::cos(r), s = std::sin(r);
    return second ? -s * u + c * v : c * u + s * v;
}

/** Frame box from width and height, the height following the image when maintainRatio is on. */
glm::dvec2 sizeBox(const ip::Settings& s, double aspect)
{
    double w = std::max(s.width, 0.0);
    return {w, s.maintainRatio ? w / aspect : std::max(s.height, 0.0)};
}

ip::Quad place(const ip::FitResult& f, const glm::dvec3& center, const glm::dvec3& right, const glm::dvec3& up, double rotation)
{
    ip::Quad q;
    q.center = center;
    q.halfU = rotateInPlane(right, up, rotation, false) * (f.size.x * 0.5);
    q.halfV = rotateInPlane(right, up, rotation, true) * (f.size.y * 0.5);
    q.uv = f.uv;
    return q;
}

void apply(Graph& g, Node& n, const Args& a)
{
    NodeId id = n.id();
    const auto& defs = n.type().attrs;
    auto setAttr = [&](int attr, PlugValue v) { g.setValue({id, attr, -1}, std::move(v)); };
    if (a.has("fileName"))
        setAttr(ImagePlane::kImageName, paths::storedPath(paths::fromUtf8(a.get<std::string>("fileName"))));
    if (a.has("camera"))
    {
        std::string cam = a.get<std::string>("camera");
        if (!ip::validCamera(cam))
            throw CommandError("imagePlane: camera is a view name (persp, top, bottom, front, back, left, right, side), a camera node or empty, not '" + cam + "'");
        setAttr(ImagePlane::kCamera, ip::cameraName(cam));
    }
    if (a.has("showInAllViews"))
        setAttr(ImagePlane::kDisplayMode, int64_t(a.get<bool>("showInAllViews") ? ip::DisplayMode::AllViews : ip::DisplayMode::LookingThrough));
    for (int attr = ImagePlane::kUseFrameExtension; attr < ImagePlane::kCount; ++attr)
    {
        const AttrDef& d = defs[size_t(attr)];
        if (attr == ImagePlane::kCamera || !a.has(d.name))
            continue;
        const json& v = a.flags[d.name];
        if (d.type == PlugType::Enum)
            setAttr(attr, enumValue(d.enumNames, v, d.name));
        else
            try
            {
                setAttr(attr, fromJson(d, v));
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError("imagePlane: " + d.name + ": " + e.what());
            }
    }
}
} // namespace

namespace imagePlane
{
FitResult fit(Fit mode, glm::dvec2 box, double imageAspect)
{
    double a = imageAspect > 0.0 && std::isfinite(imageAspect) ? imageAspect : 1.0;
    box = glm::max(box, glm::dvec2(0.0));
    double boxAspect = box.y > 0.0 ? box.x / box.y : a;
    FitResult r;
    r.size = box;
    switch (mode)
    {
    case Fit::Fill:
        if (a > boxAspect)
        {
            double w = boxAspect / a;
            r.uv = {0.5 - w * 0.5, 0.0, 0.5 + w * 0.5, 1.0};
        }
        else
        {
            double h = a / boxAspect;
            r.uv = {0.0, 0.5 - h * 0.5, 1.0, 0.5 + h * 0.5};
        }
        break;
    case Fit::Best: r.size = a > boxAspect ? glm::dvec2(box.x, box.x / a) : glm::dvec2(box.y * a, box.y); break;
    case Fit::Horizontal: r.size = {box.x, box.x / a}; break;
    case Fit::Vertical: r.size = {box.y * a, box.y}; break;
    case Fit::ToSize: break;
    }
    return r;
}

std::array<glm::dvec3, 4> Quad::corners() const
{
    return {center - halfU + halfV, center + halfU + halfV, center + halfU - halfV, center - halfU - halfV};
}

glm::dvec3 Quad::normal() const
{
    glm::dvec3 n = glm::cross(halfU, halfV);
    double len = glm::length(n);
    return len > 0.0 ? n / len : glm::dvec3(0, 0, 1);
}

Settings settings(const Node& n)
{
    Settings s;
    s.fit = Fit(std::clamp<int64_t>(n.as<int64_t>(ImagePlane::kFit), 0, int64_t(Fit::ToSize)));
    s.width = n.as<double>(ImagePlane::kWidth);
    s.height = n.as<double>(ImagePlane::kHeight);
    s.maintainRatio = n.as<bool>(ImagePlane::kMaintainRatio);
    s.offset = {n.as<double>(ImagePlane::kOffsetX), n.as<double>(ImagePlane::kOffsetY)};
    s.depth = n.as<double>(ImagePlane::kDepth);
    s.rotation = n.as<double>(ImagePlane::kRotation);
    return s;
}

Quad layoutFree(const Settings& s, const glm::dmat4& world, double imageAspect)
{
    FitResult f = fit(s.fit, sizeBox(s, imageAspect > 0.0 ? imageAspect : 1.0), imageAspect);
    glm::dvec3 center(world * glm::dvec4(s.offset, 0.0, 1.0));
    return place(f, center, glm::dvec3(world[0]), glm::dvec3(world[1]), s.rotation);
}

Quad layoutAttached(const Settings& s, const CameraFrame& cam, double imageAspect)
{
    double aspect = imageAspect > 0.0 ? imageAspect : 1.0;
    glm::dvec3 right = glm::normalize(cam.right), up = glm::normalize(cam.up), forward = glm::normalize(cam.forward);
    glm::dvec3 shift = right * s.offset.x + up * s.offset.y;
    if (cam.ortho)
        return place(fit(s.fit, sizeBox(s, aspect), aspect), forward * s.depth + shift, right, up, s.rotation);
    double depth = std::max(s.depth, 1e-3);
    glm::dvec2 box = s.fit == Fit::ToSize ? sizeBox(s, aspect) : cam.film * (2.0 * depth);
    return place(fit(s.fit, box, aspect), cam.eye + forward * depth + shift, right, up, s.rotation);
}

std::string frameFile(const std::string& path, int64_t frame)
{
    size_t nameStart = path.find_last_of("/\\");
    nameStart = nameStart == std::string::npos ? 0 : nameStart + 1;
    auto padded = [frame](size_t width) {
        std::string digits = std::to_string(frame < 0 ? -frame : frame);
        if (digits.size() < width)
            digits.insert(0, width - digits.size(), '0');
        return (frame < 0 ? "-" : "") + digits;
    };

    size_t hashEnd = path.find_last_of('#');
    if (hashEnd != std::string::npos && hashEnd >= nameStart)
    {
        size_t hashStart = hashEnd;
        while (hashStart > nameStart && path[hashStart - 1] == '#')
            --hashStart;
        return path.substr(0, hashStart) + padded(hashEnd - hashStart + 1) + path.substr(hashEnd + 1);
    }

    size_t dot = path.find_last_of('.');
    size_t stemEnd = dot == std::string::npos || dot < nameStart ? path.size() : dot;
    size_t end = stemEnd;
    while (end > nameStart && !std::isdigit(static_cast<unsigned char>(path[end - 1])))
        --end;
    if (end == nameStart)
        return path;
    size_t start = end;
    while (start > nameStart && std::isdigit(static_cast<unsigned char>(path[start - 1])))
        --start;
    return path.substr(0, start) + padded(end - start) + path.substr(end);
}

std::string cameraName(const std::string& value)
{
    std::string v = lower(value);
    if (v == "side")
        return "right";
    // Camera node names keep their case; only the views' names are case blind.
    return std::ranges::any_of(kViews, [&](const char* view) { return v == view; }) ? v : value;
}

bool validCamera(const std::string& value)
{
    std::string v = cameraName(value);
    if (v.empty() || std::ranges::any_of(kViews, [&](const char* view) { return v == view; }))
        return true;
    const std::vector<NodeId> found = Graph::current().findAll(v);
    return found.size() == 1 && Graph::current().node(found[0])->type().isA("camera");
}
} // namespace imagePlane

void registerImagePlaneTypes()
{
    if (types::find("imagePlane"))
        return;
    NodeType t;
    t.name = "imagePlane";
    t.icon = "photo";
    t.base = types::find("transform");
    t.dag = true;
    t.ownAttrs = imagePlaneAttrs();
    t.create = [] { return std::make_unique<ImagePlane>(); };
    types::registerType(std::move(t));
}

void registerImagePlaneCommands()
{
    if (ops::find("imagePlane"))
        return;
    std::vector<FlagDef> flags = {
        {"fileName", "f", T::String, nullptr, "Image file; stored relative to the workspace root (or the scene folder without one)."},
        {"camera", "c", T::String, nullptr, "View to follow: persp, front, side (right), top, ..., or a camera node; empty for a free plane."},
        {"name", "n", T::String, nullptr, "Name; made unique."},
        {"showInAllViews", "sia", T::Bool, nullptr, "Show an attached plane in every view, not only through its camera."},
        {"skipSelect", "ss", T::Bool, false, "Don't select the new plane."},
        {"useFrameExtension", "ufe", T::Bool, nullptr, "Image sequence following the time slider."},
        {"frameOffset", "fo", T::Int, nullptr, "Added to the current frame for the sequence file."},
        {"fit", "fi", T::String, nullptr, "How the image fills its frame.", kFitNames},
        {"width", "w", T::Float, nullptr, "Frame width, cm."},
        {"height", "h", T::Float, nullptr, "Frame height, cm (with maintainRatio off)."},
        {"maintainRatio", "mr", T::Bool, nullptr, "Height follows the image's aspect."},
        {"offsetX", "ox", T::Float, nullptr, "Shift right within the plane, cm."},
        {"offsetY", "oy", T::Float, nullptr, "Shift up within the plane, cm."},
        {"depth", "d", T::Float, nullptr, "Distance from a perspective eye, or behind the origin along an orthographic view, cm."},
        {"rotation", "ro", T::Float, nullptr, "Degrees around the plane's normal."},
        {"alphaGain", "ag", T::Float, nullptr, "Opacity, 0 to 1."},
        {"brightness", "br", T::Float, nullptr, "Added to the displayed color."},
        {"contrast", "ct", T::Float, nullptr, "Scales the displayed color around mid gray."},
        {"displayColor", "dc", T::String, nullptr, "color or grayscale.", kDisplayColorNames},
        {"displayMode", "dm", T::String, nullptr, "lookingThrough or allViews.", kDisplayModeNames},
        {"placement", "pl", T::String, nullptr, "behind, inScene or inFront of the geometry.", kPlacementNames},
        {"locked", "lk", T::Bool, nullptr, "Not pickable in the viewport (reference mode)."},
    };
    ops::registerCommand({
        .name = "imagePlane",
        .doc = "Create an image plane for modelling from reference images, or edit the image plane named. Returns its name.",
        .positionalDoc = "imagePlane to edit (omit to create one)",
        .flags = std::move(flags),
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            Node* n = nullptr;
            if (!a.positional.empty())
            {
                std::string target = a.positional[0].get<std::string>();
                n = g.find(target);
                if (!n || !n->type().isA("imagePlane"))
                    throw CommandError("imagePlane: " + target + " is not an image plane");
            }
            else
            {
                try
                {
                    n = g.createNode(*types::find("imagePlane"), a.get<std::string>("name", "imagePlane"));
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(std::string("imagePlane: ") + e.what());
                }
            }
            try
            {
                apply(g, *n, a);
            }
            catch (const CommandError&)
            {
                // A bad flag leaves no half-made plane behind.
                if (a.positional.empty())
                    g.deleteNode(n->id());
                throw;
            }
            if (a.positional.empty() && !a.get<bool>("skipSelect", false))
                g.select({n->id()});
            return n->path();
        },
    });
}
} // namespace lila::scene
