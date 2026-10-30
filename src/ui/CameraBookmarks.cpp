#include "ui/CameraBookmarks.h"

#include "core/Events.h"
#include "ui/Actions.h"
#include "ui/ViewportWidgetDetail.h"

#include <cmath>
#include <map>

namespace lila::ui
{
namespace
{
using json = nlohmann::json;

constexpr const char* kSection = "cameraBookmarks";

/** Built-in view name to its slots ({"1": view, ...}), saved as the scene's "cameraBookmarks" section. */
std::map<std::string, json>& builtInBookmarks()
{
    static std::map<std::string, json> m;
    return m;
}

std::function<ViewportWidget*()>& activeProvider()
{
    static std::function<ViewportWidget*()> f;
    return f;
}

json vecJson(const glm::dvec3& v)
{
    return json::array({v.x, v.y, v.z});
}

bool readNumber(const json& j, const char* key, double& out)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number() || !std::isfinite(it->get<double>()))
        return false;
    out = it->get<double>();
    return true;
}

bool readVec(const json& j, const char* key, glm::dvec3& out)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 3)
        return false;
    for (int i = 0; i < 3; ++i)
        if (!(*it)[i].is_number() || !std::isfinite((*it)[i].get<double>()))
            return false;
    out = {(*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>()};
    return true;
}

json bookmarkJson(const CameraView& v)
{
    if (v.node)
        return {{"translate", vecJson(v.translate)}, {"rotate", vecJson(v.rotate)}, {"scale", vecJson(v.scale)},
                {"orthoWidth", v.orthoWidth},        {"distance", v.distance}};
    return {{"target", vecJson(v.target)}, {"distance", v.distance}, {"yaw", v.yaw}, {"pitch", v.pitch}, {"orthoWidth", v.orthoWidth}};
}

std::optional<CameraView> bookmarkView(const json& j, bool node)
{
    if (!j.is_object())
        return std::nullopt;
    CameraView v;
    v.node = node;
    bool ok = readNumber(j, "orthoWidth", v.orthoWidth) && readNumber(j, "distance", v.distance);
    if (node)
        ok = ok && readVec(j, "translate", v.translate) && readVec(j, "rotate", v.rotate) && readVec(j, "scale", v.scale);
    else
        ok = ok && readVec(j, "target", v.target) && readNumber(j, "yaw", v.yaw) && readNumber(j, "pitch", v.pitch);
    if (!ok || v.distance <= 0.0 || v.orthoWidth <= 0.0)
        return std::nullopt;
    return v;
}

json storedBookmarks(scene::NodeId camera, const std::string& viewName)
{
    if (const scene::Node* n = scene::Graph::current().node(camera))
    {
        json j = json::parse(n->as<std::string>(scene::Camera::kBookmarks), nullptr, false);
        return j.is_object() ? j : json::object();
    }
    auto it = builtInBookmarks().find(viewName);
    return it != builtInBookmarks().end() ? it->second : json::object();
}

void storeBookmarks(scene::NodeId camera, const std::string& viewName, const json& slots)
{
    scene::Graph& g = scene::Graph::current();
    if (g.node(camera))
    {
        // Like camera moves, bookmarks stay off the scene's undo stack.
        UndoStack& u = UndoStack::get();
        bool was = u.enabled();
        u.setEnabled(false);
        g.setValue({camera, scene::Camera::kBookmarks, -1}, slots.empty() ? std::string() : slots.dump());
        u.setEnabled(was);
        io::markModified();
        return;
    }
    if (slots.empty())
        builtInBookmarks().erase(viewName);
    else
        builtInBookmarks()[viewName] = slots;
    io::uiChanged(kSection);
}

ViewportWidget* requireView()
{
    ViewportWidget* v = cameraBookmarks::activeView();
    if (!v)
        throw CommandError("cameraBookmark: no viewport");
    return v;
}
} // namespace

void ViewportWidget::setBookmark(int slot)
{
    commitCameraEdit();
    json slots = storedBookmarks(mLookThrough, mViewName);
    slots[std::to_string(slot)] = bookmarkJson(cameraView());
    storeBookmarks(mLookThrough, mViewName, slots);
}

bool ViewportWidget::goToBookmark(int slot)
{
    commitCameraEdit();
    std::optional<CameraView> v = bookmarkView(storedBookmarks(mLookThrough, mViewName).value(std::to_string(slot), json()),
                                               mLookThrough != scene::kNoNode);
    if (!v)
        return false;
    if (mLookThrough != scene::kNoNode && !beginCameraEdit())
        return false;
    v->view = historyKey();
    CameraView before = cameraView();
    applyCameraView(*v);
    recordCamera(before);
    commitCameraEdit();
    return true;
}

void ViewportWidget::clearBookmarks(int slot)
{
    json slots = json::object();
    if (slot != 0)
    {
        slots = storedBookmarks(mLookThrough, mViewName);
        slots.erase(std::to_string(slot));
    }
    storeBookmarks(mLookThrough, mViewName, slots);
}

std::vector<int> ViewportWidget::bookmarks() const
{
    json slots = storedBookmarks(mLookThrough, mViewName);
    std::vector<int> out;
    for (int slot = 1; slot <= cameraBookmarks::kSlots; ++slot)
        if (bookmarkView(slots.value(std::to_string(slot), json()), mLookThrough != scene::kNoNode))
            out.push_back(slot);
    return out;
}

namespace cameraBookmarks
{
ViewportWidget* activeView()
{
    return activeProvider() ? activeProvider()() : nullptr;
}

void install(std::function<ViewportWidget*()> active)
{
    activeProvider() = std::move(active);
    io::registerUiSection(
        kSection,
        []() -> json {
            if (builtInBookmarks().empty())
                return nullptr;
            json j = json::object();
            for (const auto& [view, slots] : builtInBookmarks())
                j[view] = slots;
            return j;
        },
        [](const json& section) {
            builtInBookmarks().clear();
            if (section.is_object())
                for (const auto& [view, slots] : section.items())
                    if (slots.is_object() && !slots.empty())
                        builtInBookmarks()[view] = slots;
        });
    events::subscribe(events::Event::SceneNew, [](const events::EventData&) { builtInBookmarks().clear(); });

    ops::registerCommand({
        .name = "cameraBookmark",
        .doc = "Jump the active view to a camera bookmark, or set, clear or list them.\n\n"
               "Each camera has its own slots 1 to 9: every camera node, and each built-in view (persp, top, front ...). "
               "Bookmarks act on the camera the active view shows. Jumping is a camera undo step, not a scene one. Returns "
               "whether it jumped, the slot set, or with query the slots in use.",
        .positionalDoc = "slot",
        .flags = {{.name = "set", .shortName = "s", .type = ArgType::Bool, .doc = "Save the view's camera in the slot instead of jumping to it."},
                  {.name = "clear", .shortName = "c", .type = ArgType::Bool, .doc = "Remove the slot's bookmark; with no slot, every one on the camera."},
                  {.name = "query", .shortName = "q", .type = ArgType::Bool, .doc = "Return the slots holding a bookmark."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            ViewportWidget* v = requireView();
            if (a.get<bool>("query"))
                return v->bookmarks();
            int slot = 0;
            if (!a.positional.empty())
            {
                if (!a.positional[0].is_number_integer() || a.positional[0].get<int>() < 1 || a.positional[0].get<int>() > kSlots)
                    throw CommandError("cameraBookmark: slot must be 1 to " + std::to_string(kSlots));
                slot = a.positional[0].get<int>();
            }
            if (a.get<bool>("clear"))
            {
                v->clearBookmarks(slot);
                return nullptr;
            }
            if (slot == 0)
                throw CommandError("cameraBookmark: slot expected");
            if (a.get<bool>("set"))
            {
                v->setBookmark(slot);
                return slot;
            }
            return v->goToBookmark(slot);
        },
        .examples = {"cameraBookmark(1, set=True)\ncameraBookmark(1)", "cameraBookmark(query=True)"},
    });

    auto call = [](json flags) {
        try
        {
            ops::execute("cameraBookmark", flags, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    };
    for (int slot = 1; slot <= kSlots; ++slot)
    {
        const std::string n = std::to_string(slot);
        actions::add({.id = "viewport.bookmarkSet" + n, .label = "Set Bookmark " + n, .category = "View",
                      .run = [call, slot] { call({{"args", {slot}}, {"set", true}}); },
                      .doc = "Saves where the active view's camera is under bookmark " + n + ". Each camera and view has its own.",
                      .python = "lila.ops.cameraBookmark(" + n + ", set=True)"});
        actions::add({.id = "viewport.bookmarkGo" + n, .label = "Bookmark " + n, .category = "View",
                      .run = [call, slot] { call({{"args", {slot}}}); },
                      .doc = "Moves the active view's camera to its bookmark " + n + ". [ goes back.",
                      .python = "lila.ops.cameraBookmark(" + n + ")"});
    }
    actions::add({.id = "viewport.bookmarkClear", .label = "Clear Bookmarks", .category = "View",
                  .run = [call] { call({{"clear", true}}); },
                  .enabled = [] { return activeView() && !activeView()->bookmarks().empty(); },
                  .doc = "Removes every bookmark on the active view's camera.",
                  .python = "lila.ops.cameraBookmark(clear=True)"});
}
} // namespace cameraBookmarks
} // namespace lila::ui
