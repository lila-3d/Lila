#include "ui/ImagePlaneUi.h"

#include "cmds/Command.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Camera.h"
#include "scene/ImagePlane.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/Camera.h"
#include "ui/TextureCache.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportWidget.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMimeData>
#include <QPolygonF>
#include <QUrl>

#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>

namespace lila::ui::imagePlanes
{
namespace
{
namespace fs = std::filesystem;
namespace ip = scene::imagePlane;
using scene::ImagePlane;

constexpr const char* kHiddenKey = "/viewport/imagePlanes/hidden";
const char* const kImageFilter = "Images (*.png *.jpg *.jpeg *.tga *.hdr *.bmp *.tif *.tiff *.webp);;All Files (*)";
const glm::vec4 kSelectedColor(0.95f, 0.95f, 0.95f, 1.0f);

std::function<ViewportWidget*()>& activeView()
{
    static std::function<ViewportWidget*()> fn;
    return fn;
}

/** The last frame each perspective view drew with, so a plane riding that camera can show in the other views. */
std::map<std::string, ip::CameraFrame>& seenCameras()
{
    static std::map<std::string, ip::CameraFrame> m;
    return m;
}

struct Showing
{
    std::string file;
    render::TexturePtr image;
};

std::unordered_map<scene::NodeId, Showing>& showing()
{
    static std::unordered_map<scene::NodeId, Showing> m;
    return m;
}

bool zUp()
{
    return PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "z";
}

/** The picture a plane shows now: its file, or the current frame of its sequence. */
render::TexturePtr imageFor(const scene::Node& n, double time)
{
    const std::string& stored = n.as<std::string>(ImagePlane::kImageName);
    if (stored.empty())
        return nullptr;
    std::string file = resolve(stored);
    bool sequence = n.as<bool>(ImagePlane::kUseFrameExtension);
    if (sequence)
        file = ip::frameFile(file, std::llround(scene::anim::frameOf(time)) + n.as<int64_t>(ImagePlane::kFrameOffset));
    render::TexturePtr t = textures::get(file, true);
    Showing& s = showing()[n.id()];
    if (t)
    {
        if (sequence && !s.file.empty() && s.file != file)
            textures::release(s.file, true);
        s.file = file;
        s.image = t;
        return t;
    }
    static std::set<std::string> warned;
    if (std::string e = textures::error(file); !e.empty() && warned.insert(file).second)
        log::warn("imagePlane {}: {}: {}", n.name(), file, e);
    // A sequence holds its last frame on screen while the next one loads.
    return sequence ? s.image : nullptr;
}

ip::CameraFrame frameOf(const ViewInfo& v)
{
    glm::dmat4 inv = glm::inverse(v.view);
    ip::CameraFrame c;
    c.right = glm::normalize(glm::dvec3(inv[0]));
    c.up = glm::normalize(glm::dvec3(inv[1]));
    c.forward = -glm::normalize(glm::dvec3(inv[2]));
    c.eye = glm::dvec3(inv[3]);
    c.ortho = v.ortho;
    c.film = v.film;
    return c;
}

/** The camera of a view name as seen from view: orthographic views have a fixed frame, persp the one it last drew. */
std::optional<ip::CameraFrame> cameraFor(const std::string& camera, const ViewInfo& view)
{
    if (camera == view.camera)
        return frameOf(view);
    scene::Graph& g = scene::Graph::current();
    if (const scene::Node* cam = g.find(camera); cam && cam->type().isA("camera"))
        return scene::camera::frame(*cam, cam->as<glm::dmat4>(scene::Transform::kWorldMatrix));
    if (camera != "persp")
    {
        Camera c;
        c.zUp = zUp();
        c.setView(camera);
        ip::CameraFrame f;
        f.right = c.right();
        f.up = c.viewUp();
        f.forward = c.forward();
        f.eye = c.eye();
        f.ortho = true;
        f.film = c.filmExtents();
        return f;
    }
    auto it = seenCameras().find(camera);
    return it == seenCameras().end() ? std::nullopt : std::optional(it->second);
}

struct Placed
{
    const scene::Node* node;
    ip::Quad quad;
    render::TexturePtr image;
};

/** Every image plane that shows in the view, laid out. */
std::vector<Placed> placedIn(const ViewInfo& view)
{
    std::vector<Placed> out;
    if (!shownIn(view.camera))
        return out;
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (!n->type().isA("imagePlane") || !scene::effectivelyVisible(g, id))
            continue;
        std::string camera = ip::cameraName(n->as<std::string>(ImagePlane::kCamera));
        std::optional<ip::CameraFrame> frame;
        if (!camera.empty())
        {
            if (camera != view.camera && ip::DisplayMode(n->as<int64_t>(ImagePlane::kDisplayMode)) != ip::DisplayMode::AllViews)
                continue;
            if (!(frame = cameraFor(camera, view)))
                continue;
        }
        render::TexturePtr image = imageFor(*n, g.time());
        if (!image || image->width <= 0 || image->height <= 0)
            continue;
        double aspect = double(image->width) / double(image->height);
        ip::Settings s = ip::settings(*n);
        ip::Quad q = frame ? ip::layoutAttached(s, *frame, aspect) : ip::layoutFree(s, n->as<glm::dmat4>(scene::Transform::kWorldMatrix), aspect);
        out.push_back({n, q, std::move(image)});
    }
    return out;
}

render::ImagePlaneDraw::Depth depthOf(const scene::Node& n)
{
    switch (ip::Placement(n.as<int64_t>(ImagePlane::kPlacement)))
    {
    case ip::Placement::InScene: return render::ImagePlaneDraw::Depth::Scene;
    case ip::Placement::InFront: return render::ImagePlaneDraw::Depth::Front;
    default: return render::ImagePlaneDraw::Depth::Behind;
    }
}

void draw(render::FrameState& s, OverlayPainter&, const ViewInfo& view)
{
    LILA_ZONE();

    if (!view.ortho)
        seenCameras()[view.camera] = frameOf(view);
    scene::Graph& g = scene::Graph::current();
    for (const Placed& p : placedIn(view))
    {
        const scene::Node& n = *p.node;
        render::ImagePlaneDraw d;
        auto corners = p.quad.corners();
        for (int i = 0; i < 4; ++i)
            d.corners[i] = glm::vec3(corners[size_t(i)]);
        d.uv = glm::vec4(p.quad.uv);
        d.image = p.image;
        d.opacity = float(n.as<double>(ImagePlane::kAlphaGain));
        d.brightness = float(n.as<double>(ImagePlane::kBrightness));
        d.contrast = float(n.as<double>(ImagePlane::kContrast));
        d.grayscale = n.as<int64_t>(ImagePlane::kDisplayColor) == 1;
        d.depth = depthOf(n);
        s.imagePlanes.push_back(std::move(d));
        if (g.isSelected(n.id()))
            for (int i = 0; i < 4; ++i)
            {
                s.linesOnTop.push_back({glm::vec3(corners[size_t(i)]), kSelectedColor});
                s.linesOnTop.push_back({glm::vec3(corners[size_t(i + 1) % 4]), kSelectedColor});
            }
    }
}

double segDistance(QPointF p, QPointF a, QPointF b)
{
    QPointF d = b - a;
    double len2 = QPointF::dotProduct(d, d);
    double t = len2 > 0.0 ? std::clamp(QPointF::dotProduct(p - a, d) / len2, 0.0, 1.0) : 0.0;
    QPointF q = a + d * t - p;
    return std::hypot(q.x(), q.y());
}

/**
 * A click picks a plane by its outline; its inside only when it draws in front, since the viewport can't tell here
 * whether a mesh is under the cursor. Locked planes never pick.
 */
std::vector<viewportHooks::PickHit> pick(QPointF a, QPointF b, bool click, const ViewInfo& view)
{
    LILA_ZONE();

    std::vector<viewportHooks::PickHit> hits;
    QRectF box = QRectF(a, b).normalized();
    for (const Placed& p : placedIn(view))
    {
        if (p.node->as<bool>(ImagePlane::kLocked))
            continue;
        QPolygonF poly;
        for (const glm::dvec3& c : p.quad.corners())
            if (auto px = view.project(c))
                poly << *px;
        if (poly.size() != 4)
            continue;
        if (!click)
        {
            if (std::ranges::any_of(poly, [&](const QPointF& c) { return box.contains(c); }))
                hits.push_back({p.node->name(), 0.0});
            continue;
        }
        double best = 1e300;
        for (int i = 0; i < 4; ++i)
            best = std::min(best, segDistance(a, poly[i], poly[(i + 1) % 4]));
        if (best < 6.0)
            hits.push_back({p.node->name(), best});
        else if (depthOf(*p.node) == render::ImagePlaneDraw::Depth::Front && poly.containsPoint(a, Qt::OddEvenFill))
            hits.push_back({p.node->name(), 5.0});
    }
    return hits;
}

void create(const std::string& file, const std::string& camera)
{
    json call = {{"fileName", file}};
    if (!camera.empty())
        call["camera"] = camera;
    try
    {
        cmds::execute("imagePlane", call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

std::vector<std::string> droppedImages(const QMimeData* mime)
{
    static const QStringList kSuffixes = {"png", "jpg", "jpeg", "tga", "hdr", "bmp", "tif", "tiff", "webp"};
    std::vector<std::string> out;
    if (!mime || !mime->hasUrls())
        return out;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile() && kSuffixes.contains(QFileInfo(url.toLocalFile()).suffix().toLower()))
            out.push_back(url.toLocalFile().toStdString());
    return out;
}

class DropFilter final : public QObject
{
public:
    explicit DropFilter(ViewportWidget* view) : QObject(view), mView(view) {}

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove)
        {
            auto* d = static_cast<QDragMoveEvent*>(e);
            if (droppedImages(d->mimeData()).empty())
                return QObject::eventFilter(obj, e);
            d->acceptProposedAction();
            return true;
        }
        if (e->type() == QEvent::Drop)
        {
            auto* d = static_cast<QDropEvent*>(e);
            std::vector<std::string> files = droppedImages(d->mimeData());
            if (files.empty())
                return QObject::eventFilter(obj, e);
            d->acceptProposedAction();
            mView->setFocus(Qt::OtherFocusReason);
            for (const std::string& f : files)
                create(f, mView->viewName());
            return true;
        }
        return QObject::eventFilter(obj, e);
    }

private:
    ViewportWidget* mView;
};
} // namespace

bool shownIn(const std::string& view)
{
    json hidden = PrefStore::get().value("general", kHiddenKey, json::array());
    return !hidden.is_array() || std::ranges::find(hidden, json(view)) == hidden.end();
}

void setShownIn(const std::string& view, bool on)
{
    json hidden = PrefStore::get().value("general", kHiddenKey, json::array());
    if (!hidden.is_array())
        hidden = json::array();
    json out = json::array();
    for (const json& v : hidden)
        if (v != json(view))
            out.push_back(v);
    if (!on)
        out.push_back(view);
    PrefStore::get().set("general", kHiddenKey, out);
}

std::string resolve(const std::string& stored)
{
    return paths::toUtf8(paths::resolvePath(stored));
}

void importImage(QWidget* parent, const std::string& camera)
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    QString file = QFileDialog::getOpenFileName(parent, camera.empty() ? "Create Image Plane" : "Import Image", start, kImageFilter);
    if (!file.isEmpty())
        create(file.toStdString(), camera);
}

void fillCameraMenu(QMenu* menu, const std::string& camera)
{
    QObject::connect(menu->addAction("Import Image..."), &QAction::triggered, menu, [menu, camera] { importImage(menu->parentWidget(), camera); });
    scene::Graph& g = scene::Graph::current();
    bool first = true;
    for (scene::NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (!n->type().isA("imagePlane") || ip::cameraName(n->as<std::string>(ImagePlane::kCamera)) != camera)
            continue;
        if (std::exchange(first, false))
            menu->addSeparator();
        std::string name = n->name();
        QObject::connect(menu->addAction(QString::fromStdString("Select " + name)), &QAction::triggered, menu, [name] {
            try
            {
                cmds::execute("select", {{"args", {name}}}, CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        });
    }
}

void acceptDrops(ViewportWidget* view)
{
    view->setAcceptDrops(true);
    view->installEventFilter(new DropFilter(view));
}

void install(std::function<ViewportWidget*()> active)
{
    activeView() = std::move(active);
    viewportHooks::addDrawer(draw);
    viewportHooks::addPicker(pick);
    onTimeChanged(qApp, [] {
        scene::Graph& g = scene::Graph::current();
        for (scene::NodeId id : g.nodes())
            if (const scene::Node* n = g.node(id); n->type().isA("imagePlane") && n->as<bool>(ImagePlane::kUseFrameExtension))
            {
                viewportTools::requestRedraw();
                return;
            }
    });

    actions::add({.id = "create.imagePlane",
                  .label = "Image Plane...",
                  .category = "Create",
                  .run = [] { importImage(QApplication::activeWindow(), {}); },
                  .doc = "Creates a free image plane from an image file, placed by its transform like any object.",
                  .python = "lila.ops.imagePlane(fileName=\"refs/side.png\")"});
    actions::add({.id = "viewport.imagePlaneImport",
                  .label = "Import Image...",
                  .category = "View",
                  .run =
                      [] {
                          ViewportWidget* v = activeView() ? activeView()() : nullptr;
                          importImage(v ? v->window() : QApplication::activeWindow(), v ? v->viewName() : "persp");
                      },
                  .doc = "Attaches a new image plane to the active view's camera.",
                  .python = "lila.ops.imagePlane(fileName=\"refs/front.png\", camera=\"front\")"});
    actions::add({.id = "viewport.toggleImagePlanes",
                  .label = "Image Planes",
                  .category = "View",
                  .run =
                      [] {
                          if (ViewportWidget* v = activeView() ? activeView()() : nullptr)
                              setShownIn(v->viewName(), !shownIn(v->viewName()));
                      },
                  .doc = "Shows or hides image planes in the active view."});
}
} // namespace lila::ui::imagePlanes
