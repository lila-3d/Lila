#include "ui/ReferenceOverlay.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "ops/Command.h"
#include "render/FrameState.h"
#include "ui/Actions.h"
#include "ui/Camera.h"
#include "ui/CursorBadges.h"
#include "ui/FileDialog.h"
#include "ui/Icons.h"
#include "ui/Overlay.h"
#include "ui/ReferenceSet.h"
#include "ui/ReferenceWindow.h"
#include "ui/TextureCache.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportToast.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportWidget.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QImageReader>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <map>
#include <set>

namespace lila::ui::referenceOverlays
{
namespace
{
namespace refs = references;
using refs::Overlay;

std::function<ViewportWidget*()>& activeViewFn()
{
    static std::function<ViewportWidget*()> fn;
    return fn;
}

ViewportWidget* activeViewWidget()
{
    return activeViewFn() ? activeViewFn()() : nullptr;
}

/** A viewport showing view, the active one first. */
ViewportWidget* viewWidget(const std::string& view)
{
    if (ViewportWidget* v = activeViewWidget(); v && v->viewName() == view)
        return v;
    for (QWidget* w : QApplication::allWidgets())
        if (auto* v = qobject_cast<ViewportWidget*>(w); v && v->viewName() == view && v->isVisible())
            return v;
    return nullptr;
}

json runCommand(const char* name, const json& call)
{
    try
    {
        return ops::execute(name, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return nullptr;
    }
}

enum class DragKind
{
    None,
    Move,
    Scale,       // right drag while O is held: by distance dragged
    ScaleCorner, // a corner handle: by distance from the centre
    Rotate,      // middle drag while O is held: by distance dragged
    RotateKnob,  // the knob: by angle around the centre
};

struct State
{
    bool keyHeld = false;
    bool keyUsed = false; // a drag or wheel turn happened while held, so the release is no tap
    bool blink = false;
    QElapsedTimer held;
    bool adjust = false;
    std::string adjustView;
    std::map<std::string, std::string> active; // view -> overlay id
    std::set<std::string> hiddenViews;
    DragKind drag = DragKind::None;
    std::string dragOverlay;
    QPointF press;
    Overlay start;
    glm::dvec2 viewSize{1.0};
    int textureListener = 0;
};

State& state()
{
    static State s;
    return s;
}

double holdDelayMs()
{
    return 180.0;
}

double aspectOf(const refs::Image& img)
{
    const std::string file = refs::fileOf(img);
    render::TexturePtr t = textures::get(file, true);
    if (t && t->width > 0 && t->height > 0)
        return double(t->width) / double(t->height);
    // Still loading: the file header has the size.
    const QSize size = QImageReader(QString::fromStdString(file)).size();
    return size.isValid() && size.height() > 0 ? double(size.width()) / double(size.height()) : 1.0;
}

bool hit(const Overlay& o, glm::dvec2 viewSize, glm::dvec2 px)
{
    const refs::Image* img = refs::current().image(o.image);
    return img && refs::overlayContains(o, img, aspectOf(*img), viewSize, px);
}

/** An overlay's on-screen corners in view pixels, with its image's aspect. */
std::array<glm::dvec2, 4> cornersOf(const Overlay& o, glm::dvec2 viewSize)
{
    const refs::Image* img = refs::current().image(o.image);
    return refs::overlayCorners(o, img, img ? aspectOf(*img) : 1.0, viewSize);
}

glm::dvec2 centreOf(const std::array<glm::dvec2, 4>& c)
{
    return (c[0] + c[1] + c[2] + c[3]) * 0.25;
}

glm::dvec3 forwardOf(const ViewInfo& view)
{
    const glm::dmat4 inv = glm::inverse(view.view);
    return -glm::normalize(glm::dvec3(inv[2]));
}

std::string activeIn(const std::string& view)
{
    const auto overlays = refs::current().overlaysIn(view);
    if (overlays.empty())
        return {};
    auto it = state().active.find(view);
    if (it != state().active.end() && refs::current().overlay(it->second) && refs::current().overlay(it->second)->view == view)
        return it->second;
    // The topmost visible one, else the topmost.
    for (auto o = overlays.rbegin(); o != overlays.rend(); ++o)
        if ((*o)->visible)
            return (*o)->id;
    return overlays.back()->id;
}

void edit(const std::string& id, const std::function<void(Overlay&)>& fn)
{
    refs::tweak([&](refs::Set& s) {
        if (Overlay* o = s.overlay(id))
            fn(*o);
    });
}

void redraw()
{
    viewportTools::requestRedraw();
}

// --- Drawing -------------------------------------------------------------------------------------------------------

/** World corners of view-pixel corners, just behind the near plane. */
void toWorld(const ViewInfo& view, const std::array<glm::dvec2, 4>& px, glm::vec3 (&out)[4])
{
    const glm::dmat4 inv = glm::inverse(view.viewProj());
    for (int k = 0; k < 4; ++k)
    {
        const glm::dvec2 ndc(px[size_t(k)].x / view.width * 2.0 - 1.0, px[size_t(k)].y / view.height * 2.0 - 1.0);
        const glm::dvec4 p = inv * glm::dvec4(ndc.x, ndc.y, 0.99, 1.0);
        out[k] = glm::vec3(glm::dvec3(p) / p.w);
    }
}

struct HudButton
{
    const char* id;
    const char* icon;
    const char* label;
};

const HudButton kHud[] = {
    {"prev", "chevron-left", "Prev"},       {"next", "chevron-right", "Next"},      {"fainter", "minus", "Fainter"},
    {"stronger", "plus", "Stronger"},       {"depth", "stack-front", "Front"},      {"mode", "photo", "Image"},
    {"flip", "flip-vertical", "Flip"},      {"ghost", "ghost", "Ghost"},          {"bind", "pin", "Bind"},
    {"goto", "camera", "Go to View"},       {"lens", "aperture", "Lens"},          {"park", "layout-sidebar-right", "Park"},
    {"remove", "trash", "Remove"},          {"done", "check", "Done"},
};
constexpr double kHudSize = 26.0, kHudGap = 6.0, kHudTop = 34.0, kKnob = 26.0;

/** Each button's slot fits its widest caption, so captions never run into each other. */
double hudSlot(int index)
{
    const std::string_view id = kHud[index].id;
    double w = OverlayPainter::textWidth(kHud[index].label);
    if (id == "depth")
        w = std::max(w, double(OverlayPainter::textWidth("Behind")));
    else if (id == "mode")
        w = std::max(w, double(OverlayPainter::textWidth("Outline")));
    return std::max(kHudSize, w + 4.0);
}

QRectF hudRect(int index, double viewWidth)
{
    double total = 0.0, before = 0.0;
    for (int i = 0; i < int(std::size(kHud)); ++i)
    {
        if (i == index)
            before = total;
        total += hudSlot(i) + kHudGap;
    }
    total -= kHudGap;
    const double x = viewWidth * 0.5 - total * 0.5 + before + (hudSlot(index) - kHudSize) * 0.5;
    return {x, kHudTop, kHudSize, kHudSize};
}

glm::dvec2 knobOf(const std::array<glm::dvec2, 4>& c)
{
    const glm::dvec2 top = (c[0] + c[1]) * 0.5, centre = centreOf(c);
    const glm::dvec2 out = glm::length(top - centre) > 1e-6 ? glm::normalize(top - centre) : glm::dvec2(0, -1);
    return top + out * kKnob;
}

QPointF qp(glm::dvec2 v)
{
    return {v.x, v.y};
}

void drawHud(OverlayPainter& p, const ViewInfo& view, const Overlay& o)
{
    const glm::dvec2 size(view.width, view.height);
    const auto c = cornersOf(o, size);
    const glm::vec4 accent(0.3f, 0.75f, 1.0f, 1.0f), white(0.95f, 0.95f, 0.95f, 1.0f), dim(0.1f, 0.1f, 0.1f, 0.75f);
    std::vector<QPointF> outline;
    for (const glm::dvec2& v : c)
        outline.push_back(qp(v));
    p.setShadow(true);
    p.polyline(outline, 1.5f, accent, true);
    for (const glm::dvec2& v : c)
        p.disc(qp(v), 5.0f, accent);
    const glm::dvec2 knob = knobOf(c);
    p.line(qp((c[0] + c[1]) * 0.5), qp(knob), 1.5f, accent);
    p.disc(qp(knob), 6.0f, accent);
    p.setShadow(false);
    for (int i = 0; i < int(std::size(kHud)); ++i)
    {
        const QRectF r = hudRect(i, view.width);
        const double pad = (hudSlot(i) - kHudSize) * 0.5;
        p.rect(r.adjusted(-3 - pad, -3, 3 + pad, 18), dim);
        std::string_view iconName = kHud[i].icon;
        QString label = kHud[i].label;
        bool on = false;
        if (std::string_view(kHud[i].id) == "depth")
        {
            iconName = o.depth == refs::Depth::Front ? "stack-front" : "stack-back";
            label = o.depth == refs::Depth::Front ? "Front" : "Behind";
        }
        else if (std::string_view(kHud[i].id) == "mode")
        {
            static const char* const kIcons[] = {"photo", "contrast", "vector"};
            static const char* const kLabels[] = {"Image", "Gray", "Outline"};
            iconName = kIcons[int(o.mode)];
            label = kLabels[int(o.mode)];
        }
        else if (std::string_view(kHud[i].id) == "ghost")
            on = o.ghost;
        else if (std::string_view(kHud[i].id) == "bind")
            on = o.bound.set;
        else if (std::string_view(kHud[i].id) == "flip")
            on = refs::current().image(o.image) && refs::current().image(o.image)->flipH;
        p.icon(r.adjusted(3, 3, -3, -3), iconName, on ? accent : white);
        p.text(QPointF(r.center().x(), r.bottom() + 14), label, white, OverlayPainter::Center);
    }
    QString info = QString("Opacity %1%   Lens %2 mm").arg(int(std::lround(o.opacity * 100))).arg(o.focalLength, 0, 'f', 0);
    if (o.bound.set)
        info += "   Bound";
    p.label(QPointF(view.width * 0.5 - OverlayPainter::textWidth(info) * 0.5, kHudTop + kHudSize + 34), info);
}

void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();

    const refs::Set& set = refs::current();
    if (set.overlays.empty())
        return;
    State& st = state();
    const bool hidden = st.hiddenViews.contains(view.camera) && !st.blink;
    const glm::dvec2 size(view.width, view.height);
    const glm::dvec3 forward = forwardOf(view);
    for (const refs::Overlay* o : set.overlaysIn(view.camera))
    {
        if (hidden || !o->visible)
            continue;
        const refs::Image* img = set.image(o->image);
        if (!img)
            continue;
        render::TexturePtr tex = textures::get(refs::fileOf(*img), true);
        if (!tex || tex->width <= 0 || tex->height <= 0)
            continue;
        const double fade = refs::boundFade(*o, forward);
        if (fade <= 0.001)
            continue;
        const double aspect = double(tex->width) / double(tex->height);
        render::ImagePlaneDraw d;
        toWorld(view, refs::overlayCorners(*o, img, aspect, size), d.corners);
        d.uv = glm::vec4(refs::overlayUv(img));
        d.image = tex;
        d.opacity = float((st.blink ? 1.0 : o->opacity) * fade);
        d.brightness = float(img->brightness);
        d.contrast = float(img->contrast);
        d.grayscale = img->grayscale || o->mode == refs::Mode::Grayscale;
        d.outline = o->mode == refs::Mode::Outline;
        d.outlineColor = o->outlineColor;
        d.depth = o->depth == refs::Depth::Front ? render::ImagePlaneDraw::Depth::Front : render::ImagePlaneDraw::Depth::Behind;
        s.imagePlanes.push_back(std::move(d));
        if (o->ghost)
            s.xray = true;
    }
    if (st.adjust && st.adjustView == view.camera)
        if (const Overlay* o = set.overlay(activeIn(view.camera)))
            drawHud(p, view, *o);
}

// --- Adjusting -----------------------------------------------------------------------------------------------------

void setAdjust(bool on, const std::string& view = {});

void cycle(const std::string& view, int step)
{
    const auto overlays = refs::current().overlaysIn(view);
    if (overlays.empty())
        return;
    const std::string cur = activeIn(view);
    int i = 0;
    for (size_t k = 0; k < overlays.size(); ++k)
        if (overlays[k]->id == cur)
            i = int(k);
    const int n = int(overlays.size());
    const std::string next = overlays[size_t(((i + step) % n + n) % n)]->id;
    state().active[view] = next;
    state().hiddenViews.erase(view);
    // Alone: the others hide, except bound ones, which show by the view's angle anyway.
    refs::tweak([&](refs::Set& s) {
        for (Overlay& o : s.overlays)
            if (o.view == view)
                o.visible = o.id == next || o.bound.set;
    });
    if (const refs::Image* img = refs::current().image(refs::current().overlay(next)->image))
        viewportToast::showActive(QString::fromStdString(img->name));
}

void bindToView(const std::string& id, bool matchLens)
{
    const Overlay* o = refs::current().overlay(id);
    ViewportWidget* v = o ? viewWidget(o->view) : nullptr;
    if (!v)
        return;
    if (v->lookThroughCamera() != scene::kNoNode)
    {
        viewportToast::showActive("Bind works on the built-in views; this view looks through a camera");
        return;
    }
    Camera& cam = v->camera();
    if (matchLens && !cam.ortho)
        cam.focalLength = o->focalLength;
    const ViewInfo info = v->viewInfo();
    edit(id, [&](Overlay& ov) {
        ov.bound.set = true;
        ov.bound.target = cam.target;
        ov.bound.distance = cam.distance;
        ov.bound.yaw = cam.yaw;
        ov.bound.pitch = cam.pitch;
        ov.bound.focalLength = cam.focalLength;
        ov.bound.ortho = cam.ortho;
        ov.bound.orthoWidth = cam.orthoWidth;
        ov.bound.forward = forwardOf(info);
    });
    v->requestFrame();
}

bool goToView(const std::string& id)
{
    const Overlay* o = refs::current().overlay(id);
    if (!o || !o->bound.set)
        return false;
    ViewportWidget* v = viewWidget(o->view);
    if (!v || v->lookThroughCamera() != scene::kNoNode)
        return false;
    Camera& cam = v->camera();
    if (o->bound.ortho)
    {
        cam.target = o->bound.target;
        if (o->bound.orthoWidth > 0.0)
            cam.orthoWidth = o->bound.orthoWidth;
    }
    else
    {
        cam.target = o->bound.target;
        cam.distance = o->bound.distance;
        cam.yaw = o->bound.yaw;
        cam.pitch = o->bound.pitch;
        cam.focalLength = o->bound.focalLength;
    }
    edit(id, [](Overlay& ov) { ov.visible = true; });
    state().hiddenViews.erase(o->view);
    v->requestFrame();
    return true;
}

void parkView(const std::string& view)
{
    ViewportWidget* v = viewWidget(view);
    const glm::dvec2 size = v ? glm::dvec2(v->width(), v->height()) : glm::dvec2(16.0, 9.0);
    refs::tweak([&](refs::Set& s) { refs::park(s, view, aspectOf, size); });
}

void hudAction(const std::string& button, const std::string& view)
{
    const std::string id = activeIn(view);
    const Overlay* o = refs::current().overlay(id);
    if (!o)
        return;
    if (button == "prev" || button == "next")
        cycle(view, button == "prev" ? -1 : 1);
    else if (button == "fainter" || button == "stronger")
        edit(id, [&](Overlay& ov) { ov.opacity = std::clamp(ov.opacity + (button == "fainter" ? -0.1 : 0.1), 0.05, 1.0); });
    else if (button == "depth")
        edit(id, [](Overlay& ov) { ov.depth = ov.depth == refs::Depth::Front ? refs::Depth::Behind : refs::Depth::Front; });
    else if (button == "mode")
        edit(id, [](Overlay& ov) { ov.mode = refs::Mode((int(ov.mode) + 1) % 3); });
    else if (button == "flip")
    {
        const refs::Image* img = refs::current().image(o->image);
        if (img)
            runCommand("referenceImage", {{"edit", img->id}, {"flipH", !img->flipH}});
    }
    else if (button == "ghost")
        edit(id, [](Overlay& ov) { ov.ghost = !ov.ghost; });
    else if (button == "bind")
    {
        if (o->bound.set)
            edit(id, [](Overlay& ov) { ov.bound = {}; });
        else
            bindToView(id, false);
    }
    else if (button == "goto")
    {
        if (!goToView(id))
            viewportToast::showActive("Bind the overlay to a view first");
    }
    else if (button == "lens")
    {
        if (ViewportWidget* v = viewWidget(view); v && !v->camera().ortho)
        {
            v->camera().focalLength = o->focalLength;
            v->requestFrame();
            viewportToast::showActive(QString("Lens set to %1 mm").arg(o->focalLength, 0, 'f', 0));
        }
    }
    else if (button == "park")
        parkView(view);
    else if (button == "remove")
        runCommand("referenceOverlay", {{"remove", json::array({id})}});
    else if (button == "done")
        setAdjust(false);
}

/** Leaves adjust mode on Esc, whichever widget has focus. */
class EscFilter final : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape && state().adjust)
        {
            setAdjust(false);
            return true;
        }
        return QObject::eventFilter(obj, e);
    }
};

void setAdjust(bool on, const std::string& view)
{
    static QPointer<EscFilter> filter;
    State& st = state();
    if (on)
    {
        if (refs::current().overlaysIn(view).empty())
        {
            viewportToast::showActive("No reference overlays in this view: drag a Reference tab onto it, or Alt+drop an image");
            return;
        }
        st.adjust = true;
        st.adjustView = view;
        st.hiddenViews.erase(view);
        if (!filter)
        {
            filter = new EscFilter(qApp);
            qApp->installEventFilter(filter);
        }
    }
    else
    {
        st.adjust = false;
        st.adjustView.clear();
        if (filter)
        {
            qApp->removeEventFilter(filter);
            filter->deleteLater();
        }
    }
    redraw();
}

bool pressDrag(QMouseEvent* e, const ViewInfo& view)
{
    State& st = state();
    const bool inAdjust = st.adjust && st.adjustView == view.camera;
    if (!st.keyHeld && !inAdjust)
        return false;
    if (e->modifiers() & Qt::AltModifier)
        return false;
    const glm::dvec2 size(view.width, view.height), px(e->position().x(), e->position().y());
    st.viewSize = size;
    if (inAdjust && e->button() == Qt::LeftButton)
    {
        for (int i = 0; i < int(std::size(kHud)); ++i)
            if (hudRect(i, view.width).adjusted(-3, -3, 3, 18).contains(e->position()))
            {
                hudAction(kHud[i].id, view.camera);
                return true;
            }
    }
    std::string id = activeIn(view.camera);
    const refs::Set& set = refs::current();
    if (inAdjust && e->button() == Qt::LeftButton)
    {
        // A click on another overlay makes it the active one.
        if (const Overlay* cur = set.overlay(id); !cur || !hit(*cur, size, px))
        {
            const auto overlays = set.overlaysIn(view.camera);
            for (auto o = overlays.rbegin(); o != overlays.rend(); ++o)
                if ((*o)->visible && hit(**o, size, px))
                {
                    id = (*o)->id;
                    st.active[view.camera] = id;
                    break;
                }
        }
    }
    const Overlay* o = set.overlay(id);
    if (!o)
        return st.keyHeld;
    st.dragOverlay = id;
    st.start = *o;
    st.press = e->position();
    st.keyUsed = true;
    st.blink = false;
    st.hiddenViews.erase(view.camera);
    if (!o->visible)
        edit(id, [](Overlay& ov) { ov.visible = true; });
    if (inAdjust && e->button() == Qt::LeftButton)
    {
        const auto c = cornersOf(*o, size);
        auto near = [&](glm::dvec2 at) { return glm::length(at - px) < 9.0; };
        if (near(knobOf(c)))
            st.drag = DragKind::RotateKnob;
        else if (std::ranges::any_of(c, near))
            st.drag = DragKind::ScaleCorner;
        else if (hit(*o, size, px))
            st.drag = DragKind::Move;
        else
            st.drag = DragKind::None;
        return true;
    }
    st.drag = e->button() == Qt::LeftButton ? DragKind::Move : e->button() == Qt::RightButton ? DragKind::Scale : e->button() == Qt::MiddleButton ? DragKind::Rotate : DragKind::None;
    return st.drag != DragKind::None || inAdjust;
}

void moveDrag(QMouseEvent* e, const ViewInfo& view)
{
    State& st = state();
    if (st.drag == DragKind::None)
        return;
    const glm::dvec2 size(view.width, view.height), px(e->position().x(), e->position().y()), press(st.press.x(), st.press.y());
    const Overlay& s0 = st.start;
    const bool snap = e->modifiers() & Qt::ShiftModifier;
    const glm::dvec2 centre = centreOf(cornersOf(s0, size));
    edit(st.dragOverlay, [&](Overlay& o) {
        switch (st.drag)
        {
        case DragKind::Move: o.offset = s0.offset + (px - press) / std::max(size.y, 1.0); break;
        case DragKind::Scale: o.scale = std::clamp(s0.scale * std::exp((px.x - press.x) / 250.0), 0.02, 20.0); break;
        case DragKind::ScaleCorner:
        {
            const double d0 = glm::length(press - centre), d1 = glm::length(px - centre);
            if (d0 > 1.0)
                o.scale = std::clamp(s0.scale * d1 / d0, 0.02, 20.0);
            break;
        }
        case DragKind::Rotate:
        case DragKind::RotateKnob:
        {
            double r = s0.rotation;
            if (st.drag == DragKind::Rotate)
                r += (px.x - press.x) * 0.4;
            else
                r += glm::degrees(std::atan2(px.y - centre.y, px.x - centre.x) - std::atan2(press.y - centre.y, press.x - centre.x));
            if (snap)
                r = std::round(r / 15.0) * 15.0;
            o.rotation = std::fmod(r, 360.0);
            break;
        }
        default: break;
        }
    });
}

void releaseDrag(QMouseEvent*, const ViewInfo&)
{
    state().drag = DragKind::None;
}

bool wheel(QWheelEvent* e, const ViewInfo& view)
{
    State& st = state();
    if (!st.keyHeld)
        return false;
    const std::string id = activeIn(view.camera);
    if (id.empty())
        return false;
    st.keyUsed = true;
    st.blink = false;
    st.hiddenViews.erase(view.camera);
    const double steps = e->angleDelta().y() / 120.0;
    edit(id, [&](Overlay& o) {
        o.visible = true;
        o.opacity = std::clamp(o.opacity + steps * 0.05, 0.02, 1.0);
    });
    viewportToast::showActive(QString("Overlay opacity %1%").arg(int(std::lround(refs::current().overlay(id)->opacity * 100))));
    return true;
}

void keyDown()
{
    State& st = state();
    if (st.keyHeld)
        return;
    st.keyHeld = true;
    st.keyUsed = false;
    st.blink = false;
    st.held.start();
    const std::string view = activeViewName();
    QTimer::singleShot(int(holdDelayMs()), qApp, [view] {
        State& s = state();
        if (s.keyHeld && !s.keyUsed && !refs::current().overlaysIn(view).empty())
        {
            s.blink = true;
            redraw();
        }
    });
    redraw();
}

void keyUp()
{
    State& st = state();
    if (!st.keyHeld)
        return;
    st.keyHeld = false;
    const bool tap = !st.keyUsed && st.held.elapsed() < holdDelayMs();
    st.blink = false;
    if (tap)
    {
        const std::string view = activeViewName();
        if (refs::current().overlaysIn(view).empty())
            viewportToast::showActive("No reference overlays in this view: drag a Reference tab onto it, or Alt+drop an image");
        else if (!st.hiddenViews.erase(view))
            st.hiddenViews.insert(view);
    }
    redraw();
}

// --- Adding --------------------------------------------------------------------------------------------------------

/** Adds image files to the set (no window tab) and shows each as an overlay in view. */
void addFiles(const std::vector<std::string>& files, const std::string& view)
{
    for (const std::string& f : files)
        runCommand("referenceOverlay", {{"args", json::array({f})}, {"view", view}});
}

void registerCommand()
{
    ops::registerCommand({
        .name = "referenceOverlay",
        .doc = "Show a reference image glued to a view's screen, or edit, remove or list overlays.\n\n"
               "The positional argument is a reference image id or an image file (added to the set without a tab). The "
               "overlay draws in front of the model at low opacity by default; depth behind puts it under the model. "
               "Placement is in view heights from the anchor, so a parked overlay stays parked when the view resizes. "
               "bind remembers the view's camera: goTo returns to it, and the overlay fades as the view turns away.",
        .positionalDoc = "image",
        .flags = {{.name = "view", .shortName = "v", .type = ArgType::String, .doc = "View name the overlay shows in; default the active view."},
                  {.name = "edit", .shortName = "e", .type = ArgType::String, .doc = "Id of the overlay the flags change."},
                  {.name = "remove", .shortName = "rm", .type = ArgType::List, .doc = "Ids of overlays to remove."},
                  {.name = "query", .shortName = "q", .type = ArgType::Bool, .doc = "Return the overlays (of view, when given) as dicts."},
                  {.name = "opacity", .shortName = "o", .type = ArgType::Float, .doc = "0 to 1.", .min = 0.0, .max = 1.0},
                  {.name = "depth", .shortName = "d", .type = ArgType::String, .doc = "Over the model or under it.", .choices = {"front", "behind"}},
                  {.name = "mode", .shortName = "m", .type = ArgType::String, .doc = "outline draws only the image's edges, leaving the model visible.",
                   .choices = {"normal", "grayscale", "outline"}},
                  {.name = "anchor", .shortName = "an", .type = ArgType::String, .doc = "The view point offset is measured from.",
                   .choices = {"center", "topLeft", "topRight", "bottomLeft", "bottomRight"}},
                  {.name = "offset", .shortName = "of", .type = ArgType::List, .doc = "[x, y] from the anchor to the image centre, in view heights, y down."},
                  {.name = "scale", .shortName = "s", .type = ArgType::Float, .doc = "Shown height in view heights.", .min = 0.01, .max = 20.0},
                  {.name = "rotation", .shortName = "r", .type = ArgType::Float, .doc = "Degrees clockwise."},
                  {.name = "visible", .shortName = "vis", .type = ArgType::Bool, .doc = "Shown or hidden."},
                  {.name = "ghost", .shortName = "gh", .type = ArgType::Bool, .doc = "Draw the meshes see-through while the overlay shows."},
                  {.name = "outlineColor", .shortName = "oc", .type = ArgType::Vec3, .doc = "Line color of outline mode, display RGB 0 to 1."},
                  {.name = "fadeAngle", .shortName = "fa", .type = ArgType::Float, .doc = "Degrees from the bound view at which it has faded out.",
                   .min = 1.0, .max = 180.0},
                  {.name = "focalLength", .shortName = "fl", .type = ArgType::Float, .doc = "The lens the photo was taken with, in mm.", .min = 1.0},
                  {.name = "bind", .shortName = "b", .type = ArgType::Bool, .doc = "Remember the view's camera now; False forgets it."},
                  {.name = "matchLens", .shortName = "ml", .type = ArgType::Bool, .doc = "With bind, set the view's lens to focalLength first."},
                  {.name = "goTo", .shortName = "g", .type = ArgType::Bool, .doc = "Move the view back to the bound camera."},
                  {.name = "park", .shortName = "p", .type = ArgType::Bool, .doc = "Lay the view's overlays out opaque down its right edge."},
                  {.name = "active", .shortName = "a", .type = ArgType::Bool, .doc = "Make it the overlay the O keys and handles act on."}},
        .run = [](const Args& a) -> json {
            const std::string view = a.has("view") ? a.get<std::string>("view") : activeViewName();
            if (a.get<bool>("query"))
            {
                json all = refs::current().toJson()["overlays"], out = json::array();
                for (json& o : all)
                    if (!a.has("view") || o["view"] == view)
                        out.push_back(std::move(o));
                return out;
            }
            if (a.has("remove"))
            {
                json ids = a.flags["remove"];
                if (ids.is_string())
                    ids = json::array({ids});
                refs::Set next = refs::current();
                for (const json& j : ids)
                {
                    const std::string id = j.get<std::string>();
                    if (!next.overlay(id))
                        throw CommandError("no reference overlay '" + id + "'");
                    const std::string image = next.overlay(id)->image;
                    std::erase_if(next.overlays, [&](const Overlay& o) { return o.id == id; });
                    // An image that only existed for this overlay goes with it.
                    if (const refs::Image* img = next.image(image); img && img->window.empty() &&
                                                                    std::ranges::none_of(next.overlays, [&](const Overlay& o) { return o.image == image; }))
                        std::erase_if(next.images, [&](const refs::Image& i) { return i.id == image; });
                }
                refs::apply(std::move(next), "Remove Reference Overlay");
                return ids;
            }
            if (a.get<bool>("park"))
            {
                parkView(view);
                return true;
            }
            std::string id;
            if (a.has("edit"))
            {
                id = a.get<std::string>("edit");
                if (!refs::current().overlay(id))
                    throw CommandError("no reference overlay '" + id + "'");
            }
            else
            {
                if (a.positional.empty() || !a.positional[0].is_string())
                    throw CommandError("give a reference image id or an image file, or edit, remove, park or query");
                std::string image = a.positional[0].get<std::string>();
                refs::Set next = refs::current();
                if (!next.image(image))
                {
                    if (!refs::isImageFile(image) || !std::filesystem::exists(paths::fromUtf8(image)))
                        throw CommandError("'" + image + "' is neither a reference image id nor an image file");
                    refs::Image img;
                    img.id = next.newImageId();
                    img.path = paths::storedPath(paths::fromUtf8(image));
                    img.name = paths::toUtf8(paths::fromUtf8(image).stem());
                    next.images.push_back(img);
                    image = img.id;
                }
                Overlay o;
                o.id = next.newOverlayId();
                o.image = image;
                o.view = view;
                id = o.id;
                next.overlays.push_back(o);
                refs::apply(std::move(next), "Add Reference Overlay");
                state().active[view] = id;
                state().hiddenViews.erase(view);
            }
            edit(id, [&](Overlay& o) {
                if (a.has("view"))
                    o.view = view;
                if (a.has("opacity"))
                    o.opacity = a.get<double>("opacity");
                if (a.has("depth"))
                    o.depth = a.get<std::string>("depth") == "behind" ? refs::Depth::Behind : refs::Depth::Front;
                if (a.has("mode"))
                {
                    const std::string m = a.get<std::string>("mode");
                    o.mode = m == "outline" ? refs::Mode::Outline : m == "grayscale" ? refs::Mode::Grayscale : refs::Mode::Normal;
                }
                if (a.has("anchor"))
                {
                    static const char* const kNames[] = {"center", "topLeft", "topRight", "bottomLeft", "bottomRight"};
                    for (int i = 0; i < 5; ++i)
                        if (a.get<std::string>("anchor") == kNames[i])
                        {
                            ViewportWidget* v = viewWidget(o.view);
                            refs::reanchor(o, refs::Anchor(i), v ? glm::dvec2(v->width(), v->height()) : glm::dvec2(16.0, 9.0));
                        }
                }
                if (a.has("offset"))
                {
                    const json& off = a.flags["offset"];
                    if (!off.is_array() || off.size() != 2)
                        throw CommandError("offset is [x, y]");
                    o.offset = {off[0].get<double>(), off[1].get<double>()};
                }
                if (a.has("scale"))
                    o.scale = a.get<double>("scale");
                if (a.has("rotation"))
                    o.rotation = a.get<double>("rotation");
                if (a.has("visible"))
                    o.visible = a.get<bool>("visible");
                if (a.has("ghost"))
                    o.ghost = a.get<bool>("ghost");
                if (a.has("outlineColor"))
                {
                    const json& c = a.flags["outlineColor"];
                    o.outlineColor = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
                }
                if (a.has("fadeAngle"))
                    o.fadeAngle = a.get<double>("fadeAngle");
                if (a.has("focalLength"))
                    o.focalLength = a.get<double>("focalLength");
                if (a.has("bind") && !a.get<bool>("bind"))
                    o.bound = {};
            });
            if (a.has("bind") && a.get<bool>("bind"))
                bindToView(id, a.get<bool>("matchLens"));
            if (a.get<bool>("active"))
                state().active[refs::current().overlay(id)->view] = id;
            if (a.get<bool>("goTo") && !goToView(id))
                throw CommandError("'" + id + "' isn't bound to a view, or no viewport shows its view");
            return id;
        },
        .examples = {"referenceOverlay(\"refs/front.jpg\", view=\"persp\", opacity=0.25)",
                     "referenceOverlay(edit=\"overlay1\", mode=\"outline\", bind=True, matchLens=True)",
                     "referenceOverlay(edit=\"overlay2\", goTo=True)"},
    });
}

void registerActions()
{
    auto viewName = [] { return activeViewName(); };
    actions::add({.id = "referenceOverlay.keyDown",
                  .label = "Reference Overlay Key (press)",
                  .category = "View",
                  .run = keyDown,
                  .doc = "Hold to move (left drag), scale (right drag), rotate (middle drag) or fade (wheel) the active "
                         "reference overlay; hold still to see it at full strength; tap to show or hide the overlays."});
    actions::add({.id = "referenceOverlay.keyUp", .label = "Reference Overlay Key (release)", .category = "View", .run = keyUp,
                  .doc = "Ends the reference overlay key hold."});
    actions::add({.id = "referenceOverlay.toggle",
                  .label = "Reference Overlays",
                  .category = "View",
                  .run =
                      [viewName] {
                          const std::string v = viewName();
                          if (!state().hiddenViews.erase(v))
                              state().hiddenViews.insert(v);
                          redraw();
                      },
                  .doc = "Shows or hides the active view's reference overlays.",
                  .checked = [viewName] { return !refs::current().overlaysIn(viewName()).empty() && !state().hiddenViews.contains(viewName()); },
                  .icon = "layers-subtract"});
    actions::add({.id = "referenceOverlay.adjust",
                  .label = "Adjust Reference Overlay",
                  .category = "View",
                  .run = [viewName] { setAdjust(!state().adjust, viewName()); },
                  .doc = "Handles and a toolbar on the active view's reference overlay: drag it, its corners or its knob. Esc leaves.",
                  .checked = [] { return state().adjust; },
                  .icon = "adjustments"});
    actions::add({.id = "referenceOverlay.next",
                  .label = "Next Reference Overlay",
                  .category = "View",
                  .run = [viewName] { cycle(viewName(), 1); },
                  .doc = "Shows the view's next reference overlay alone (front, side, three-quarter...).",
                  .icon = "chevron-right"});
    actions::add({.id = "referenceOverlay.bindView",
                  .label = "Bind Reference Overlay to View",
                  .category = "View",
                  .run =
                      [viewName] {
                          if (std::string id = activeIn(viewName()); !id.empty())
                              bindToView(id, false);
                      },
                  .doc = "Remembers the view's camera for the active overlay, which then fades as the view turns away from it.",
                  .icon = "pin"});
    actions::add({.id = "referenceOverlay.goToView",
                  .label = "Go to Reference Overlay View",
                  .category = "View",
                  .run =
                      [viewName] {
                          if (std::string id = activeIn(viewName()); id.empty() || !goToView(id))
                              viewportToast::showActive("Bind the overlay to a view first");
                      },
                  .doc = "Moves the view back to the camera the active overlay is bound to.",
                  .icon = "camera"});
    actions::add({.id = "referenceOverlay.park",
                  .label = "Park Reference Overlays",
                  .category = "View",
                  .run = [viewName] { parkView(viewName()); },
                  .doc = "Lays the view's overlays out opaque down its right edge, beside the model.",
                  .icon = "layout-sidebar-right"});
    actions::add({.id = "referenceOverlay.addImage",
                  .label = "Add Reference Overlay...",
                  .category = "View",
                  .run =
                      [viewName] {
                          QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
                          const fileDialog::ContextScope scope("referenceImages", "Images");
                          QStringList files = fileDialog::getOpenFileNames(QApplication::activeWindow(), "Add Reference Overlay", start,
                                                                           "Images (*.png *.jpg *.jpeg *.tga *.hdr *.exr *.bmp *.tif *.tiff *.webp)");
                          std::vector<std::string> paths;
                          for (const QString& f : files)
                              paths.push_back(f.toStdString());
                          addFiles(paths, viewName());
                      },
                  .doc = "Opens images as reference overlays in the active view.",
                  .python = "referenceOverlay(\"refs/front.jpg\")",
                  .icon = "photo-plus"});
}
} // namespace

std::string activeViewName()
{
    ViewportWidget* v = activeViewWidget();
    return v ? v->viewName() : std::string("persp");
}

std::string show(const std::string& imageId, const std::string& view)
{
    json call = {{"args", json::array({imageId})}};
    if (!view.empty())
        call["view"] = view;
    json r = runCommand("referenceOverlay", call);
    return r.is_string() ? r.get<std::string>() : std::string();
}

std::string activeOverlay(const std::string& view)
{
    return activeIn(view);
}

bool adjusting()
{
    return state().adjust;
}

QWidget* createSculptSection(QWidget* parent)
{
    auto* box = new QGroupBox("Reference Overlay", parent);
    auto* col = new QVBoxLayout(box);
    auto* list = new QListWidget(box);
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(56, 56));
    list->setFlow(QListView::LeftToRight);
    list->setWrapping(true);
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setFixedHeight(84);
    list->setToolTip("The active view's overlays: click one to make it the one the O keys act on, double-click to show it alone");
    col->addWidget(list);
    auto* form = new QFormLayout;
    auto* opacity = new QSlider(Qt::Horizontal, box);
    opacity->setRange(2, 100);
    form->addRow("Opacity", opacity);
    auto* depth = new QComboBox(box);
    depth->addItems({"In Front", "Behind"});
    form->addRow("Depth", depth);
    auto* mode = new QComboBox(box);
    mode->addItems({"Image", "Grayscale", "Outline"});
    form->addRow("Show", mode);
    col->addLayout(form);
    auto* row = new QHBoxLayout;
    auto button = [box, row](const char* iconName, const char* tip, const char* action, bool checkable = false) {
        auto* b = new QToolButton(box);
        b->setIcon(icon(iconName));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setCheckable(checkable);
        if (*action)
            QObject::connect(b, &QToolButton::clicked, box, [action] { actions::run(json(action)); });
        row->addWidget(b);
        return b;
    };
    button("photo-plus", "Add images as overlays in the active view", "referenceOverlay.addImage");
    QToolButton* adjust = button("adjustments", "Adjust: handles and a toolbar on the overlay (Shift+O)", "referenceOverlay.adjust", true);
    button("pin", "Bind to this view: it fades as the view turns away", "referenceOverlay.bindView");
    button("camera", "Go back to the bound view", "referenceOverlay.goToView");
    button("layout-sidebar-right", "Park the overlays opaque down the right edge", "referenceOverlay.park");
    QToolButton* ghost = button("ghost", "See-through meshes while this overlay shows", "", true);
    auto* remove = new QToolButton(box);
    remove->setIcon(icon("trash"));
    remove->setAutoRaise(true);
    remove->setToolTip("Remove the overlay");
    row->addWidget(remove);
    row->addStretch(1);
    col->addLayout(row);
    auto* help = new QLabel("O: tap to hide, hold to compare, hold + drag to move, right-drag scale, wheel opacity", box);
    help->setWordWrap(true);
    help->setEnabled(false);
    col->addWidget(help);

    auto busy = std::make_shared<bool>(false);
    auto refresh = [=] {
        if (*busy)
            return;
        *busy = true;
        const std::string view = activeViewName();
        const refs::Set& set = refs::current();
        const auto overlays = set.overlaysIn(view);
        const std::string act = activeIn(view);
        list->clear();
        for (const Overlay* o : overlays)
        {
            const refs::Image* img = set.image(o->image);
            auto* item = new QListWidgetItem(img ? QString::fromStdString(img->name) : QString("?"), list);
            item->setData(Qt::UserRole, QString::fromStdString(o->id));
            if (img)
                if (QImage shown = references::displayImage(*img); !shown.isNull())
                    item->setIcon(QIcon(QPixmap::fromImage(shown.scaled(56, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
            if (!o->visible)
                item->setForeground(QColor(120, 120, 120));
            if (o->id == act)
                list->setCurrentItem(item);
        }
        const Overlay* o = set.overlay(act);
        for (QWidget* w : std::initializer_list<QWidget*>{opacity, depth, mode, ghost, remove})
            w->setEnabled(o != nullptr);
        if (o)
        {
            opacity->setValue(int(std::lround(o->opacity * 100)));
            depth->setCurrentIndex(o->depth == refs::Depth::Front ? 0 : 1);
            mode->setCurrentIndex(int(o->mode));
            ghost->setChecked(o->ghost);
        }
        adjust->setChecked(state().adjust);
        *busy = false;
    };
    const int listener = refs::addListener([refresh] { refresh(); });
    QObject::connect(box, &QObject::destroyed, [listener] { refs::removeListener(listener); });
    // The active view changes with focus, which no listener reports: a light poll keeps the list on it.
    auto* poll = new QTimer(box);
    poll->setInterval(500);
    auto lastView = std::make_shared<std::string>();
    QObject::connect(poll, &QTimer::timeout, box, [=] {
        if (!box->isVisible())
            return;
        if (std::string v = activeViewName(); v != *lastView || adjust->isChecked() != state().adjust)
        {
            *lastView = v;
            refresh();
        }
    });
    poll->start();
    auto activeId = [] { return activeIn(activeViewName()); };
    QObject::connect(list, &QListWidget::itemClicked, box, [=](QListWidgetItem* item) {
        state().active[activeViewName()] = item->data(Qt::UserRole).toString().toStdString();
        refresh();
        redraw();
    });
    QObject::connect(list, &QListWidget::itemDoubleClicked, box, [=](QListWidgetItem* item) {
        const std::string id = item->data(Qt::UserRole).toString().toStdString(), view = activeViewName();
        state().active[view] = id;
        state().hiddenViews.erase(view);
        refs::tweak([&](refs::Set& s) {
            for (Overlay& o : s.overlays)
                if (o.view == view)
                    o.visible = o.id == id || o.bound.set;
        });
    });
    QObject::connect(opacity, &QSlider::valueChanged, box, [=](int v) {
        if (!*busy)
            edit(activeId(), [v](Overlay& o) { o.opacity = v / 100.0; });
    });
    QObject::connect(depth, &QComboBox::currentIndexChanged, box, [=](int i) {
        if (!*busy)
            edit(activeId(), [i](Overlay& o) { o.depth = i == 0 ? refs::Depth::Front : refs::Depth::Behind; });
    });
    QObject::connect(mode, &QComboBox::currentIndexChanged, box, [=](int i) {
        if (!*busy)
            edit(activeId(), [i](Overlay& o) { o.mode = refs::Mode(i); });
    });
    QObject::connect(ghost, &QToolButton::toggled, box, [=](bool on) {
        if (!*busy)
            edit(activeId(), [on](Overlay& o) { o.ghost = on; });
    });
    QObject::connect(remove, &QToolButton::clicked, box, [=] {
        if (std::string id = activeId(); !id.empty())
            runCommand("referenceOverlay", {{"remove", json::array({id})}});
    });
    refresh();
    return box;
}

void install(std::function<ViewportWidget*()> activeView)
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    activeViewFn() = std::move(activeView);
    registerCommand();
    registerActions();
    for (const HudButton& b : kHud)
        overlayIcons::add(b.icon, {.tabler = b.icon});
    for (const char* name : {"stack-back", "contrast", "vector"})
        overlayIcons::add(name, {.tabler = name});
    viewportHooks::addDrawer("lila.referenceOverlays", draw);
    viewportHooks::setDragHook("referenceOverlays", {.press = pressDrag, .move = moveDrag, .release = releaseDrag});
    viewportHooks::setWheelHook("referenceOverlays", wheel);
    // A tab dragged from a Reference window onto a view shows there; Shift swaps the active overlay's image instead.
    viewportHooks::setDropHandler("referenceOverlays", [](const QMimeData& mime, const viewportHooks::DropTarget& t, bool commit) {
        if (!mime.hasFormat(references::kDragMime))
            return false;
        if (!commit)
            return true;
        const json payload = json::parse(mime.data(references::kDragMime).toStdString(), nullptr, false);
        const std::string image = payload.is_object() ? payload.value("image", "") : std::string();
        if (!refs::current().image(image))
            return true;
        const std::string view = t.view.camera;
        if (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier)
            if (std::string id = activeIn(view); !id.empty())
            {
                refs::Set next = refs::current();
                next.overlay(id)->image = image;
                refs::apply(std::move(next), "Replace Reference Overlay Image");
                return true;
            }
        runCommand("referenceOverlay", {{"args", json::array({image})}, {"view", view}});
        return true;
    });
    state().textureListener = textures::addListener([] {
        if (!refs::current().overlays.empty())
            redraw();
    });
    refs::addListener(redraw);
    cursorBadges::add({.id = "referenceOverlay.key",
                       .icon = "layers-subtract",
                       .active = [](const ViewportState& v) { return state().keyHeld && !refs::current().overlaysIn(v.view.camera).empty(); },
                       .message = "Reference overlay: drag to move, right-drag to scale, middle-drag to rotate, wheel for opacity",
                       .actions = {"referenceOverlay.keyDown"}});
}
} // namespace lila::ui::referenceOverlays
