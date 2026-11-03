// The Spinner: Puppeteer's radial transform hub. Three coloured zones drag X, Y and Z (vertically: up increases), the
// middle mouse button turns the selection freely as a trackball, and the ring holds the tool and option buttons and,
// in Animate mode, the breakdown slider. On an IK or reverse foot controller the zones become rings around a centre
// disc that swings the pole, and right-dragging the rings rolls, twists and banks the foot. Every drag previews with
// undo off and commits one Puppeteer command on release.

#include "gizmo/Spinner.h"
#include "Puppeteer.h"
#include "gizmo/ControllerDraw.h"
#include "rig/AnimProxy.h"
#include "rig/Rig.h"

#include <lila/Gizmo.h>
#include <lila/Prefs.h>
#include <lila/UI.h>
#include <lila/Undo.h>
#include <lila/Viewport.h>
#include <lila/ViewportTool.h>

#include <QActionGroup>
#include <QCursor>
#include <QKeyEvent>
#include <QMenu>

#include <cmath>
#include <map>
#include <optional>
#include <tuple>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace puppeteer
{
namespace
{
enum class Tool
{
    Move,
    Rotate,
    Scale,
};

enum class Space
{
    Local,
    World,
    Screen,
};

/** What the Spinner remembers between drags and sessions, shared by every view's instance. */
struct Settings
{
    Tool tool = Tool::Rotate;
    bool inherit = false;     // rotate: the children keep their world orientation
    bool toon = false;        // scale: the other axes compensate and the children keep their size
    double toonExponent = 1.0 / 3.0;
    Space space = Space::Local;
    bool pivot = false;       // rig: move a joint without its children
    bool discrete = false;
    bool stickyAim = false;   // rig: orients follow the bones while joints move
    bool sticky2nd = false;
    bool keepDirection = false;
    bool footAlign = false;   // the reverse foot context button aligns (levels) instead of snapping
    bool fullBody = true;     // animate: moving a joint poses the whole body around its pins
    bool autoPose = true;     // full-body moves let the auto-poser place the unpinned body
    bool help = false;
    double moveStep = 5.0;
    double rotateStep = 15.0;
    double scaleStep = 0.1;
    double rotateSpeed = 0.5;     // degrees per pixel, joints
    double controllerSpeed = 0.5; // degrees per pixel, IK and reverse foot controllers
    double poleSpeed = 0.42;      // degrees per pixel, the centre disc's pole swing
    double scaleSpeed = 0.0046;   // scale per pixel
    double trackballSpeed = 0.5;  // degrees per pixel
    double breakdownDeadZone = 20.0;
    double breakdownFull = 100.0;
    double size = 1.0;
    /** Where each view (by camera name) has the Spinner, relative to the view's centre. */
    std::map<std::string, QPointF> offsets;

    template <class T> static void read(const PrefStore& p, const char* key, T& v) { v = p.value<T>(kPrefs, key, v); }

    void load()
    {
        const PrefStore& p = PrefStore::get();
        read(p, "/spinner/inheritRotate", inherit);
        read(p, "/spinner/toonScale", toon);
        read(p, "/spinner/toonExponent", toonExponent);
        space = Space(std::clamp(p.value<int>(kPrefs, "/spinner/axes", 0), 0, 2));
        read(p, "/spinner/pivot", pivot);
        read(p, "/spinner/discrete", discrete);
        read(p, "/spinner/stickyAim", stickyAim);
        read(p, "/spinner/sticky2nd", sticky2nd);
        read(p, "/spinner/keepDirection", keepDirection);
        read(p, "/spinner/footAlign", footAlign);
        read(p, "/spinner/fullBody", fullBody);
        read(p, "/spinner/autoPose", autoPose);
        read(p, "/spinner/moveStep", moveStep);
        read(p, "/spinner/rotateStep", rotateStep);
        read(p, "/spinner/scaleStep", scaleStep);
        read(p, "/spinner/rotateSpeed", rotateSpeed);
        read(p, "/spinner/controllerSpeed", controllerSpeed);
        read(p, "/spinner/poleSpeed", poleSpeed);
        read(p, "/spinner/scaleSpeed", scaleSpeed);
        read(p, "/spinner/trackballSpeed", trackballSpeed);
        read(p, "/spinner/breakdownDeadZone", breakdownDeadZone);
        read(p, "/spinner/breakdownFull", breakdownFull);
        size = std::clamp(p.value<double>(kPrefs, "/spinner/size", 1.0), 0.5, 2.0);
        offsets.clear();
        const json o = p.value(kPrefs, "/spinner/offsets", json::object());
        if (o.is_object())
            for (const auto& [view, xy] : o.items())
                if (xy.is_array() && xy.size() == 2 && xy[0].is_number() && xy[1].is_number())
                    offsets[view] = QPointF(xy[0].get<double>(), xy[1].get<double>());
    }

    void save() const
    {
        PrefStore& p = PrefStore::get();
        p.set(kPrefs, "/spinner/inheritRotate", inherit);
        p.set(kPrefs, "/spinner/toonScale", toon);
        p.set(kPrefs, "/spinner/toonExponent", toonExponent);
        p.set(kPrefs, "/spinner/axes", int(space));
        p.set(kPrefs, "/spinner/pivot", pivot);
        p.set(kPrefs, "/spinner/discrete", discrete);
        p.set(kPrefs, "/spinner/stickyAim", stickyAim);
        p.set(kPrefs, "/spinner/sticky2nd", sticky2nd);
        p.set(kPrefs, "/spinner/keepDirection", keepDirection);
        p.set(kPrefs, "/spinner/footAlign", footAlign);
        p.set(kPrefs, "/spinner/fullBody", fullBody);
        p.set(kPrefs, "/spinner/autoPose", autoPose);
        p.set(kPrefs, "/spinner/moveStep", moveStep);
        p.set(kPrefs, "/spinner/rotateStep", rotateStep);
        p.set(kPrefs, "/spinner/scaleStep", scaleStep);
        p.set(kPrefs, "/spinner/rotateSpeed", rotateSpeed);
        p.set(kPrefs, "/spinner/controllerSpeed", controllerSpeed);
        p.set(kPrefs, "/spinner/poleSpeed", poleSpeed);
        p.set(kPrefs, "/spinner/scaleSpeed", scaleSpeed);
        p.set(kPrefs, "/spinner/trackballSpeed", trackballSpeed);
        p.set(kPrefs, "/spinner/breakdownDeadZone", breakdownDeadZone);
        p.set(kPrefs, "/spinner/breakdownFull", breakdownFull);
        p.set(kPrefs, "/spinner/size", size);
        json o = json::object();
        for (const auto& [view, xy] : offsets)
            o[view] = json::array({xy.x(), xy.y()});
        p.set(kPrefs, "/spinner/offsets", o);
    }
};

Settings& settings()
{
    static Settings s = [] {
        Settings x;
        x.load();
        return x;
    }();
    return s;
}

void changed()
{
    settings().save();
    viewportTools::requestRedraw();
}

void setTool(Tool t)
{
    settings().tool = t;
    actions::run(t == Tool::Move ? "tool.move" : t == Tool::Rotate ? "tool.rotate" : "tool.scale");
    changed();
}

/** O and P: switch to the tool, or toggle its alternative when already in it. */
void toolOrAlternative(Tool t)
{
    Settings& s = settings();
    if (s.tool == t)
    {
        if (t == Tool::Rotate)
            s.inherit = !s.inherit;
        else if (t == Tool::Scale)
            s.toon = !s.toon;
    }
    setTool(t);
}

enum class Handle
{
    None,
    Plate, // the rim and the ring between buttons: dragging it moves the Spinner
    AxisX,
    AxisY,
    AxisZ,
    Center,
    Move,
    Rotate,
    Scale,
    Space,
    Discrete,
    Pivot,
    Context,
    Sticky2nd,
    KeepDirection,
    Settings,
    Help,
    Proxy,
    Breakdown,
};

bool isAxis(Handle h) { return h == Handle::AxisX || h == Handle::AxisY || h == Handle::AxisZ; }
int axisOf(Handle h) { return h == Handle::AxisX ? 0 : h == Handle::AxisY ? 1 : 2; }

/** Where the Spinner sits by default: right of the view's centre, logical pixels. */
constexpr double kDefaultOffsetX = 250.0;
/** The layout is measured on a 424 px canvas centred at 212: everything scales by radius() / 212. */
constexpr double kCanvas = 212.0;
constexpr double kZoneOuter = 143.0;
constexpr double kRingInner = 66.0;
constexpr double kCenterDisc = 59.0;
constexpr double kFreeDot = 24.0;
constexpr double kButtonRing = 177.0;
constexpr double kButtonSize = 22.0;
// The zones in screen degrees (y down): red X on top, green Y lower left, blue Z lower right.
constexpr double kZoneX[2] = {-150.0, -30.0};
constexpr double kZoneY[2] = {90.0, 210.0};
constexpr double kZoneZ[2] = {-30.0, 90.0};
constexpr double kPlate = 202.0;
constexpr double kTabRadius = 226.0;
// Screen angles in degrees: 0 to the right, 90 straight down.
constexpr double kBreakdownFrom = 48.0;
constexpr double kBreakdownTo = 133.0;
constexpr double kBreakdownInner = 155.0;
constexpr double kBreakdownOuter = 199.0;
constexpr double kGizmoPixels = 70.0;

struct Button
{
    Handle handle;
    double angle;
    double radius = kButtonRing;
};

constexpr Button kButtons[] = {
    {Handle::Move, -120.0},      {Handle::Rotate, -90.0},       {Handle::Scale, -60.0},     {Handle::Space, 0.0},
    {Handle::Discrete, 30.0},    {Handle::Pivot, 180.0},        {Handle::Context, 150.0},   {Handle::Sticky2nd, 120.0},
    {Handle::KeepDirection, 96.0}, {Handle::Settings, -40.0, kTabRadius}, {Handle::Help, -26.0, kTabRadius},
    {Handle::Proxy, -12.0, kTabRadius},
};

glm::vec4 gray(float v, float a = 1.0f) { return {v, v, v, a}; }
const glm::vec4 kOrange{0.93f, 0.55f, 0.13f, 1.0f};
const glm::vec4 kYellow{0.92f, 0.78f, 0.22f, 1.0f};

std::vector<std::string> selectedTransforms()
{
    Graph& g = graph();
    std::vector<std::string> out;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("transform"))
            out.push_back(n->path());
    return out;
}

json namesJson(const std::vector<std::string>& names)
{
    json a = json::array();
    for (const std::string& n : names)
        a.push_back(n);
    return a;
}

/** Runs with undo recording off, putting it back however fn ends. Returns what fn threw, empty when nothing. */
template <class Fn> std::string quietly(Fn&& fn)
{
    UndoStack& u = UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    // Previews read joints over and over; the skinned meshes update once, when the viewport draws.
    const Graph::DeferMeshes defer;
    std::string error;
    try
    {
        fn();
    }
    catch (const std::exception& e)
    {
        error = e.what();
    }
    u.setEnabled(was);
    return error;
}

/** What is selected, as the Spinner sees it. */
struct Target
{
    std::vector<std::string> names;
    NodeId chain = kNoNode; // the IK chain whose controller is selected
    NodeId foot = kNoNode;  // its reverse foot
    bool rig = false;       // Rig mode
};

Target currentTarget()
{
    Graph& g = graph();
    Target t;
    t.names = selectedTransforms();
    t.rig = modes::current() == kRigMode;
    for (NodeId id : g.selection())
    {
        Node* n = g.node(id);
        if (!n)
            continue;
        // Only a controller is posed through its chain; its joints themselves turn FK.
        if (NodeId c = lila::scene::rigel::chainOfController(g, id))
        {
            t.chain = c;
            t.foot = footForChain(g, c);
            break;
        }
    }
    return t;
}

/** Off a key, in Animate mode, the Spinner greys out: the selection has keys but none at the current time. */
bool offKey(const std::vector<std::string>& names)
{
    if (names.empty() || modes::current() != kAnimateMode)
        return false;
    Graph& g = graph();
    static uint64_t version = ~0ull;
    static double time = 0.0;
    static std::vector<std::string> cachedNames;
    static bool cached = false;
    if (version == g.version() && time == g.time() && cachedNames == names)
        return cached;
    version = g.version();
    time = g.time();
    cachedNames = names;
    cached = false;
    try
    {
        const json times = ops::execute("keyframe", {{"args", namesJson(names)}, {"query", true}});
        bool any = false, here = false;
        auto visit = [&](const json& j, auto& self) -> void {
            if (j.is_number())
            {
                any = true;
                here |= std::abs(j.get<double>() - g.time()) < 1e-6;
            }
            else if (j.is_array())
                for (const json& e : j)
                    self(e, self);
        };
        visit(times, visit);
        cached = any && !here;
    }
    catch (const std::exception&)
    {
    }
    return cached;
}

QString tipFor(Handle h, const Target& t)
{
    const Settings& s = settings();
    const bool rings = t.chain && s.tool == Tool::Rotate;
    switch (h)
    {
    case Handle::AxisX:
    case Handle::AxisY:
    case Handle::AxisZ:
    {
        static const char* axes[] = {"X", "Y", "Z"};
        static const char* foot[] = {"roll", "twist", "bank"};
        QString tip = QString("Drag up or down: %1. Middle drag: free rotate").arg(axes[axisOf(h)]);
        if (rings && t.foot)
            tip += QString(". Right drag: %1").arg(foot[axisOf(h)]);
        return tip;
    }
    case Handle::Center:
        if (rings)
            return "Drag up or down: swing the pole. Right drag: swing the pole and turn the controller with it";
        if (s.tool == Tool::Scale)
            return "Drag up or down: scale every axis";
        return s.tool == Tool::Move ? "Drag: move in the screen plane" : "Drag up or down: turn around the view";
    case Handle::Move: return "Move (I)";
    case Handle::Rotate:
        return s.inherit ? "Inherit Rotate: the children keep their world orientation (right-click: Rotate) (O)"
                         : "Rotate (right-click: Inherit Rotate) (O)";
    case Handle::Scale:
        return s.toon ? "Toon Scale: the other axes compensate (right-click: Scale) (P)" : "Scale (right-click: Toon Scale) (P)";
    case Handle::Space:
        return s.space == Space::Local ? "Local space (8). Click: world" : s.space == Space::World ? "World space (9). Click: screen" : "Screen space (0). Click: local";
    case Handle::Discrete: return s.discrete ? "Discrete steps on" : "Discrete steps off";
    case Handle::Pivot: return s.pivot ? "Pivot: move joints without their children" : "Pivot off: joints carry their children";
    case Handle::Context:
        if (t.foot)
            return s.footAlign ? "Reverse Foot Align: level the controller keeping the pose (right-click: Snap)"
                               : "Reverse Foot Snap: put the controller on the sole keeping the pose (right-click: Align)";
        return s.stickyAim ? "Sticky Aim on: orients follow the bones as joints move" : "Sticky Aim off";
    case Handle::Sticky2nd: return s.sticky2nd ? "Sticky 2nd on: the secondary axis lies in the chain's plane" : "Sticky 2nd off";
    case Handle::KeepDirection: return s.keepDirection ? "Keep Direction on: the secondary axis keeps its sense" : "Keep Direction off";
    case Handle::Settings: return "Spinner settings";
    case Handle::Help: return "Show what each part of the Spinner does";
    case Handle::Proxy:
        return selectionShowsProxy() ? "Anim proxy shown: click for the real meshes. Right-click: the selected joint's real piece"
                                     : "Real meshes shown: click for the anim proxy. Right-click: the selected joint's real piece";
    case Handle::Breakdown:
        return "Breakdown: drag left toward the previous key's pose, right toward the next (Shift: the whole character)";
    case Handle::Plate: return "Drag the rim to move the Spinner (reset in its settings)";
    default: return {};
    }
}

class SpinnerGizmo final : public Gizmo
{
public:
    void setView(const ViewInfo& view) override { mView = view; }

    bool supports(std::string_view mode) const override
    {
        if (mode != mLastMode)
        {
            mLastMode = std::string(mode);
            if (mode == "move")
                settings().tool = Tool::Move;
            else if (mode == "rotate")
                settings().tool = Tool::Rotate;
            else if (mode == "scale")
                settings().tool = Tool::Scale;
        }
        return mode == "select" || mode == "move" || mode == "rotate" || mode == "scale";
    }

    bool visible() override { return !selectedTransforms().empty(); }

    void draw(OverlayPainter& p) override
    {
        if (!mDrag && !visible())
            return;
        const Target t = currentTarget();
        const Settings& s = settings();
        const bool disabled = offKey(t.names);
        drawViewportGizmo(p, t);

        const QPointF c = center();
        const Handle hot = mDrag ? mDrag->handle : mHover;
        p.disc(c, float(u(kPlate)), gray(0.14f, 0.92f), 72);

        // Zones.
        const bool color = viewportDisplay::accentColors(viewportDisplay::kManipulatorAxisColors);
        const bool rings = ringLayout(t);
        const bool footChannel = mDrag && mDrag->kind == Kind::FootChannel;
        const double zones[3][2] = {{kZoneX[0], kZoneX[1]}, {kZoneY[0], kZoneY[1]}, {kZoneZ[0], kZoneZ[1]}};
        const double inner = rings ? u(kRingInner) : 0.0;
        for (int axis = 0; axis < 3; ++axis)
        {
            glm::vec4 col = color ? glm::vec4(viewportDisplay::axisColor(axis), 1.0f) : gray(0.62f - 0.1f * float(axis));
            if (footChannel)
                col = color ? kOrange : gray(0.7f);
            const Handle h = axis == 0 ? Handle::AxisX : axis == 1 ? Handle::AxisY : Handle::AxisZ;
            if (hot == h)
                col = glm::mix(col, glm::vec4(1.0f), 0.35f);
            if (disabled)
                col = gray(0.32f);
            fillZone(p, c, inner, u(kZoneOuter), zones[axis][0], zones[axis][1], col);
            if (footChannel)
            {
                const double mid = math::radians((zones[axis][0] + zones[axis][1]) * 0.5);
                const double r = (inner + u(kZoneOuter)) * 0.5;
                iconAt(p, QPointF(c.x() + r * std::cos(mid), c.y() + r * std::sin(mid)), u(34.0), "puppeteer.foot", gray(0.1f));
            }
        }
        for (double a : {-150.0, -30.0, 90.0})
            p.line(at(c, inner, a), at(c, u(kZoneOuter) + 1.0, a), float(u(5.0)), gray(0.14f));

        // Centre: the IK/foot pole disc, the uniform scale disc, or Lila's small free-drag dot.
        if (rings)
        {
            glm::vec4 col = !color ? gray(0.78f) : s.tool == Tool::Scale ? kYellow : kOrange;
            if (hot == Handle::Center)
                col = glm::mix(col, glm::vec4(1.0f), 0.3f);
            if (disabled)
                col = gray(0.32f);
            p.disc(c, float(u(kCenterDisc)), col, 40);
            iconAt(p, c, u(60.0), s.tool == Tool::Scale ? "puppeteer.local" : "puppeteer.pole", gray(0.1f));
        }
        else
        {
            p.disc(c, float(u(kFreeDot)), disabled ? gray(0.32f) : hot == Handle::Center ? gray(0.98f) : gray(0.8f), 24);
        }

        // Settings and help sit on a tab outside the rim, which grows to hold the anim proxy toggle.
        const bool proxyTab = shown(Handle::Proxy, t);
        p.disc(at(c, u(kTabRadius), proxyTab ? -26.0 : -33.0), float(u(proxyTab ? 44.0 : 30.0)), gray(0.14f, 0.92f), 24);
        for (const Button& b : kButtons)
            if (shown(b.handle, t))
                drawButton(p, b, t);

        if (modes::current() != kRigMode && !t.rig)
            drawBreakdown(p, c, hot, disabled);

        if (mDrag && !mDrag->readout.isEmpty())
            p.label(QPointF(c.x() - u(kPlate), c.y() - u(kPlate) - 10.0), mDrag->readout);
        else if (!mDrag && mHover != Handle::None)
            p.label(QPointF(std::max(8.0, c.x() - u(kPlate) - 160.0), c.y() - u(kPlate) - 10.0), tipFor(mHover, t));
        if (s.help && !mDrag)
            drawHelp(p, c);
    }

    bool hover(QPointF px) override
    {
        const Handle h = visible() ? hit(px, currentTarget()) : Handle::None;
        if (h == mHover)
            return false;
        mHover = h;
        return true;
    }

    bool wantsRightButton(QPointF px) override
    {
        if (visible() && hit(px, currentTarget()) != Handle::None)
            return true;
        return controllerAt(graph(), mView, px) != kNoNode;
    }

    bool press(QPointF px, Qt::MouseButton button, Qt::KeyboardModifiers mods) override
    {
        const Target t = currentTarget();
        const Handle h = visible() ? hit(px, t) : Handle::None;
        if (h == Handle::None)
        {
            // Right-clicking an IK or reverse foot controller in the view locks or unlocks its key.
            if (button == Qt::RightButton)
                if (NodeId c = controllerAt(graph(), mView, px))
                {
                    runCommand("puppeteerFootLock", {{"args", {graph().node(c)->path()}}});
                    viewportTools::requestRedraw();
                    return true;
                }
            return false;
        }
        Settings& s = settings();
        const bool right = button == Qt::RightButton;
        switch (h)
        {
        case Handle::Move:
            if (!right)
                setTool(Tool::Move);
            return true;
        case Handle::Rotate:
            if (right)
                s.inherit = !s.inherit;
            setTool(Tool::Rotate);
            return true;
        case Handle::Scale:
            if (right)
                s.toon = !s.toon;
            setTool(Tool::Scale);
            return true;
        case Handle::Space:
            s.space = Space((int(s.space) + (right ? 2 : 1)) % 3);
            changed();
            return true;
        case Handle::Discrete:
            s.discrete = !s.discrete;
            changed();
            return true;
        case Handle::Pivot:
            s.pivot = !s.pivot;
            changed();
            return true;
        case Handle::Context:
            if (t.foot)
            {
                if (right)
                {
                    s.footAlign = !s.footAlign;
                    changed();
                }
                else
                    runCommand("puppeteerSnapReverseFoot", {{"args", {graph().node(t.foot)->path()}}, {"align", s.footAlign}});
            }
            else
            {
                s.stickyAim = !s.stickyAim;
                changed();
            }
            return true;
        case Handle::Sticky2nd:
            s.sticky2nd = !s.sticky2nd;
            changed();
            return true;
        case Handle::KeepDirection:
            s.keepDirection = !s.keepDirection;
            changed();
            return true;
        case Handle::Settings: openSettings(); return true;
        case Handle::Proxy:
            if (right)
                toggleSelectedRealPiece();
            else
                toggleSelectedProxy();
            return true;
        case Handle::Help:
            s.help = !s.help;
            changed();
            return true;
        case Handle::Plate:
            if (button == Qt::LeftButton)
                beginDrag(Kind::Reposition, h, px, mods, t);
            return true;
        case Handle::Breakdown:
            if (button == Qt::LeftButton && !offKey(t.names))
                beginDrag(Kind::Breakdown, h, px, mods, t);
            return true;
        default: break;
        }
        // The zones and the centre.
        if (offKey(t.names))
            return true;
        const bool rings = ringLayout(t);
        if (button == Qt::MiddleButton)
            beginDrag(Kind::Trackball, h, px, mods, t);
        else if (right)
        {
            if (h == Handle::Center && rings && s.tool == Tool::Rotate)
                beginDrag(Kind::PoleTurn, h, px, mods, t);
            else if (isAxis(h) && rings && t.foot && s.tool == Tool::Rotate)
                beginDrag(Kind::FootChannel, h, px, mods, t);
        }
        else if (button == Qt::LeftButton)
        {
            if (h == Handle::Center && rings && s.tool == Tool::Rotate)
                beginDrag(Kind::Pole, h, px, mods, t);
            else
                beginDrag(Kind::Transform, h, px, mods, t);
        }
        return true;
    }

    void drag(QPointF px, Qt::KeyboardModifiers mods) override
    {
        (void)mods;
        if (!mDrag)
            return;
        mDrag->current = px;
        apply(false);
    }

    void release() override
    {
        if (!mDrag)
            return;
        apply(true);
        mDrag.reset();
    }

    void cancel() override
    {
        if (!mDrag)
            return;
        restore();
        mDrag.reset();
    }

    bool dragging() const override { return mDrag.has_value(); }
    bool hovering() const override { return mHover != Handle::None; }

    bool keyPress(QKeyEvent* e) override
    {
        if (mDrag && e->key() == Qt::Key_Escape)
        {
            cancel();
            return true;
        }
        return false;
    }

    QString readout() const override { return mDrag ? mDrag->readout : QString(); }
    void warped(QPointF offset) override
    {
        if (mDrag)
            mDrag->start -= offset;
    }
    bool wrapsCursor() const override { return !mDrag || (mDrag->kind != Kind::Breakdown && mDrag->kind != Kind::Reposition); }

private:
    enum class Kind
    {
        Transform,   // zones and the plain centre: move, rotate or scale
        Pole,        // centre disc on a controller: the pole alone
        PoleTurn,    // the pole with the controller turning along
        FootChannel, // right drag on a zone: roll, twist or bank
        Trackball,   // middle drag: free rotate
        Breakdown,
        Reposition,  // the rim: moves the Spinner
    };

    struct Saved
    {
        NodeId id;
        std::vector<std::pair<int, PlugValue>> values;
    };

    struct Drag
    {
        Kind kind = Kind::Transform;
        Handle handle = Handle::None;
        Tool tool = Tool::Rotate;
        QPointF start, current;
        bool all = false;
        Target target;
        std::vector<Saved> saved;
        glm::dvec3 pivot{0.0};
        glm::dmat3 axes{1.0};
        std::vector<BreakdownSample> samples;
        double weight = 0.0;
        QPointF offset, startOffset;
        QString readout;
        bool warned = false;
    };

    double u(double canvas) const { return canvas * radius() / kCanvas; }
    double radius() const { return 120.0 * settings().size; }

    bool ringLayout(const Target& t) const
    {
        const Settings& s = settings();
        return s.tool == Tool::Scale || (t.chain && s.tool == Tool::Rotate);
    }

    QPointF offset() const
    {
        if (mDrag && mDrag->kind == Kind::Reposition)
            return mDrag->offset;
        const auto& o = settings().offsets;
        const auto it = o.find(mView.camera);
        return it != o.end() ? it->second : QPointF(kDefaultOffsetX, 0.0);
    }

    /** Kept wholly inside the view however small it gets. */
    QPointF center() const
    {
        const double r = u(kTabRadius + 20.0);
        const QPointF c = QPointF(mView.width * 0.5, mView.height * 0.5) + offset();
        return {std::clamp(c.x(), r, std::max(r, mView.width - r)), std::clamp(c.y(), r, std::max(r, mView.height - r))};
    }

    static QPointF at(QPointF c, double r, double degrees)
    {
        const double a = math::radians(degrees);
        return {c.x() + r * std::cos(a), c.y() + r * std::sin(a)};
    }

    static void iconAt(OverlayPainter& p, QPointF c, double size, const char* name, const glm::vec4& color)
    {
        p.icon(QRectF(c.x() - size * 0.5, c.y() - size * 0.5, size, size), name, color);
    }

    static void fillZone(OverlayPainter& p, QPointF c, double inner, double outer, double from, double to, const glm::vec4& col)
    {
        // Convex pieces: thin wedges or ring segments.
        constexpr int kSteps = 24;
        for (int k = 0; k < kSteps; ++k)
        {
            const double a0 = from + (to - from) * k / kSteps, a1 = from + (to - from) * (k + 1) / kSteps;
            if (inner <= 0.0)
                p.fill({c, at(c, outer, a0), at(c, outer, a1)}, col);
            else
                p.fill({at(c, inner, a0), at(c, outer, a0), at(c, outer, a1), at(c, inner, a1)}, col);
        }
    }

    bool shown(Handle h, const Target& t) const
    {
        const Settings& s = settings();
        switch (h)
        {
        case Handle::Pivot: return t.rig;
        case Handle::Context: return t.foot || t.rig;
        case Handle::Sticky2nd:
        case Handle::KeepDirection: return t.rig && s.stickyAim;
        case Handle::Proxy: return animProxyEnabled() && !selectedProxySets().empty();
        default: return true;
        }
    }

    bool active(Handle h) const
    {
        const Settings& s = settings();
        switch (h)
        {
        case Handle::Move: return s.tool == Tool::Move;
        case Handle::Rotate: return s.tool == Tool::Rotate;
        case Handle::Scale: return s.tool == Tool::Scale;
        case Handle::Discrete: return s.discrete;
        case Handle::Pivot: return s.pivot;
        case Handle::Context: return !currentTarget().foot && s.stickyAim;
        case Handle::Sticky2nd: return s.sticky2nd;
        case Handle::KeepDirection: return s.keepDirection;
        case Handle::Help: return s.help;
        case Handle::Proxy: return selectionShowsProxy();
        default: return false;
        }
    }

    /** The buttons with an alternative on the right button carry a small corner box. */
    static bool hasAlternative(Handle h)
    {
        return h == Handle::Rotate || h == Handle::Scale || h == Handle::Context || h == Handle::Space || h == Handle::Proxy;
    }

    const char* iconFor(Handle h, const Target& t) const
    {
        const Settings& s = settings();
        switch (h)
        {
        case Handle::Move: return "puppeteer.move";
        case Handle::Rotate: return s.inherit ? "puppeteer.inherit" : "puppeteer.rotate";
        case Handle::Scale: return s.toon ? "puppeteer.toon" : "puppeteer.scale";
        case Handle::Space: return s.space == Space::World ? "puppeteer.world" : s.space == Space::Local ? "puppeteer.local" : "puppeteer.screen";
        case Handle::Discrete: return "puppeteer.discrete";
        case Handle::Pivot: return "puppeteer.pivot";
        case Handle::Context: return t.foot ? (s.footAlign ? "puppeteer.footAlign" : "puppeteer.foot") : "puppeteer.sticky";
        case Handle::Sticky2nd: return "puppeteer.sticky2nd";
        case Handle::KeepDirection: return "puppeteer.keepDirection";
        case Handle::Settings: return "puppeteer.settings";
        case Handle::Help: return "puppeteer.help";
        case Handle::Proxy: return "puppeteer.proxy";
        default: return "";
        }
    }

    void drawButton(OverlayPainter& p, const Button& b, const Target& t)
    {
        const QPointF pos = at(center(), u(b.radius), b.angle);
        const double br = u(kButtonSize);
        const bool on = active(b.handle);
        const bool hot = mHover == b.handle;
        if (on)
            p.disc(pos, float(br), gray(0.88f), 20);
        else if (hot)
            p.disc(pos, float(br), gray(0.32f), 20);
        iconAt(p, pos, br * 1.7, iconFor(b.handle, t), on ? gray(0.1f) : hot ? gray(1.0f) : gray(0.8f));
        if (hasAlternative(b.handle))
        {
            const double k = br * 0.28;
            const QPointF corner = pos + QPointF(br * 0.62, -br * 0.62);
            p.rect(QRectF(corner.x() - k * 0.5, corner.y() - k * 0.5, k, k), on ? gray(0.1f) : gray(0.72f));
        }
    }

    void drawBreakdown(OverlayPainter& p, QPointF c, Handle hot, bool disabled)
    {
        const double r = u((kBreakdownInner + kBreakdownOuter) * 0.5);
        std::vector<QPointF> arc;
        for (int k = 0; k <= 32; ++k)
            arc.push_back(at(c, r, kBreakdownFrom + (kBreakdownTo - kBreakdownFrom) * k / 32.0));
        p.polyline(arc, float(u(6.0)), gray(disabled ? 0.25f : 0.42f));
        // The dot follows the drag along the arc, back to the middle once released.
        double dot = 90.0;
        if (mDrag && mDrag->kind == Kind::Breakdown)
            dot = std::clamp(90.0 - (mDrag->current.x() - mDrag->start.x()) / u(kBreakdownOuter) * 45.0, kBreakdownFrom, kBreakdownTo);
        p.disc(at(c, r, dot), float(u(12.0)), disabled ? gray(0.35f) : hot == Handle::Breakdown ? gray(1.0f) : gray(0.85f), 20);
    }

    void drawHelp(OverlayPainter& p, QPointF c)
    {
        const QStringList lines = {"Drag a zone up or down: X, Y, Z",
                                   "Middle drag: free rotate",
                                   "Controller centre: pole (right: pole and controller)",
                                   "Reverse foot, right drag a ring: roll, twist, bank",
                                   "Right-click a controller in the view: lock its key",
                                   "Right-click a button with a corner box: its alternative",
                                   "Drag the rim: move the Spinner"};
        double y = c.y() + u(kPlate) + 18.0;
        for (const QString& l : lines)
        {
            p.label(QPointF(c.x() - u(kPlate), y), l);
            y += 18.0;
        }
    }

    /** The joint-side gizmo: axis rings (rotate) or lines (move, scale) at the pivot, white on the hovered or dragged axis. */
    void drawViewportGizmo(OverlayPainter& p, const Target& t)
    {
        if (t.names.empty())
            return;
        Graph& g = graph();
        Node* n = g.find(t.names.front());
        if (!n)
            return;
        const glm::dvec3 pivot = mDrag ? mDrag->pivot : glm::dvec3(worldOf(g, n->id())[3]);
        const glm::dmat3 axes = mDrag ? mDrag->axes : axesFor(g, n->id());
        const double len = kGizmoPixels * mView.unitsPerPixel(pivot);
        const bool color = viewportDisplay::accentColors(viewportDisplay::kManipulatorAxisColors);
        const Handle hot = mDrag ? mDrag->handle : mHover;
        if (mDrag && mDrag->kind == Kind::Trackball)
        {
            if (auto at = mView.project(pivot))
            {
                p.disc(*at, float(kGizmoPixels), gray(0.85f, 0.16f), 48);
                std::vector<QPointF> ring;
                for (int k = 0; k <= 48; ++k)
                    ring.push_back(*at + QPointF(kGizmoPixels * std::cos(2.0 * math::kPi * k / 48.0), kGizmoPixels * std::sin(2.0 * math::kPi * k / 48.0)));
                p.polyline(ring, 1.5f, gray(0.9f, 0.6f));
            }
            return;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            const bool lit = isAxis(hot) && axisOf(hot) == axis;
            if (mDrag && mDrag->kind == Kind::Transform && !lit)
                continue;
            const glm::vec4 col = lit ? gray(1.0f) : color ? glm::vec4(viewportDisplay::axisColor(axis), 0.9f) : gray(0.6f);
            std::vector<QPointF> pts;
            if (settings().tool == Tool::Rotate)
            {
                const glm::dvec3 a = axes[(axis + 1) % 3], b = axes[(axis + 2) % 3];
                for (int k = 0; k <= 48; ++k)
                {
                    const double ang = 2.0 * math::kPi * k / 48.0;
                    if (auto s = mView.project(pivot + (a * std::cos(ang) + b * std::sin(ang)) * len))
                        pts.push_back(*s);
                }
            }
            else
                for (double f : {0.0, 1.0})
                    if (auto s = mView.project(pivot + axes[axis] * len * f))
                        pts.push_back(*s);
            if (pts.size() > 1)
                p.polyline(pts, lit ? 2.5f : 1.5f, col);
        }
    }

    glm::dmat3 axesFor(Graph& g, NodeId first) const
    {
        switch (settings().space)
        {
        case Space::World: return glm::dmat3(1.0);
        case Space::Local: return math::rotationOf(worldOf(g, first));
        case Space::Screen: return glm::transpose(glm::dmat3(mView.view));
        }
        return glm::dmat3(1.0);
    }

    Handle hit(QPointF px, const Target& t) const
    {
        const QPointF c = center();
        const double dx = px.x() - c.x(), dy = px.y() - c.y();
        const double d = std::hypot(dx, dy);
        for (const Button& b : kButtons)
            if (shown(b.handle, t))
            {
                const QPointF bp = at(c, u(b.radius), b.angle);
                if (std::hypot(px.x() - bp.x(), px.y() - bp.y()) <= u(kButtonSize) * 1.15)
                    return b.handle;
            }
        if (d > u(kPlate))
            return Handle::None;
        const double a = std::atan2(dy, dx) * 180.0 / math::kPi;
        const bool rings = ringLayout(t);
        if (rings ? d <= u(kCenterDisc) : d <= u(kFreeDot))
            return Handle::Center;
        if (d <= u(kZoneOuter))
        {
            if (rings && d < u(kRingInner))
                return Handle::Plate;
            return a >= kZoneX[0] && a <= kZoneX[1] ? Handle::AxisX : a > kZoneZ[0] && a <= kZoneZ[1] ? Handle::AxisZ : Handle::AxisY;
        }
        if (!t.rig && modes::current() != kRigMode && a >= kBreakdownFrom && a <= kBreakdownTo && d >= u(kBreakdownInner) &&
            d <= u(kBreakdownOuter))
            return Handle::Breakdown;
        return Handle::Plate;
    }

    void openSettings()
    {
        auto* m = new QMenu;
        m->setAttribute(Qt::WA_DeleteOnClose);
        Settings& s = settings();
        auto toggle = [m](const QString& label, bool& flag) {
            QAction* a = m->addAction(label);
            a->setCheckable(true);
            a->setChecked(flag);
            QObject::connect(a, &QAction::toggled, [&flag](bool on) {
                flag = on;
                changed();
            });
        };
        auto choice = [m](const QString& title, double& value, std::initializer_list<std::pair<QString, double>> options) {
            QMenu* sub = m->addMenu(title);
            auto* group = new QActionGroup(sub);
            for (const auto& [label, v] : options)
            {
                QAction* a = sub->addAction(label);
                a->setCheckable(true);
                a->setChecked(std::abs(value - v) < 1e-9);
                group->addAction(a);
                const double pick = v;
                QObject::connect(a, &QAction::triggered, [&value, pick] {
                    value = pick;
                    changed();
                });
            }
        };
        toggle("Discrete Steps", s.discrete);
        choice("Move Step", s.moveStep, {{"1 cm", 1.0}, {"5 cm", 5.0}, {"10 cm", 10.0}, {"25 cm", 25.0}});
        choice("Rotate Step", s.rotateStep, {{"5°", 5.0}, {"15°", 15.0}, {"45°", 45.0}, {"90°", 90.0}});
        choice("Scale Step", s.scaleStep, {{"0.05", 0.05}, {"0.1", 0.1}, {"0.25", 0.25}});
        m->addSeparator();
        choice("Joint Rotate Speed", s.rotateSpeed, {{"0.25°/px", 0.25}, {"0.43°/px", 0.43}, {"0.5°/px", 0.5}, {"1°/px", 1.0}});
        choice("Controller Rotate Speed", s.controllerSpeed, {{"0.25°/px", 0.25}, {"0.5°/px", 0.5}, {"0.62°/px", 0.62}, {"1°/px", 1.0}});
        choice("Pole Speed", s.poleSpeed, {{"0.25°/px", 0.25}, {"0.42°/px", 0.42}, {"0.62°/px", 0.62}, {"1°/px", 1.0}});
        choice("Toon Scale Coefficient", s.toonExponent, {{"0 (none)", 0.0}, {"1/3", 1.0 / 3.0}, {"1/2 (volume)", 0.5}, {"1", 1.0}});
        choice("Breakdown Full Travel", s.breakdownFull, {{"60 px", 60.0}, {"100 px", 100.0}, {"160 px", 160.0}});
        m->addSeparator();
        toggle("Inherit Rotate", s.inherit);
        toggle("Toon Scale", s.toon);
        toggle("Full Body Move", s.fullBody);
        toggle("Auto Pose", s.autoPose);
        toggle("Sticky Aim (rigging)", s.stickyAim);
        toggle("Pivot (rigging)", s.pivot);
        m->addSeparator();
        choice("Size", s.size, {{"Small", 0.75}, {"Medium", 1.0}, {"Large", 1.35}});
        const std::string view = mView.camera;
        QObject::connect(m->addAction("Reset Position"), &QAction::triggered, [view] {
            settings().offsets.erase(view);
            changed();
        });
        QObject::connect(m->addAction("Reset Position in Every View"), &QAction::triggered, [] {
            settings().offsets.clear();
            changed();
        });
        m->popup(QCursor::pos());
    }

    void beginDrag(Kind kind, Handle h, QPointF px, Qt::KeyboardModifiers mods, const Target& t)
    {
        Graph& g = graph();
        Drag d;
        d.kind = kind;
        d.handle = h;
        d.tool = settings().tool;
        d.start = d.current = px;
        d.target = t;
        d.all = mods.testFlag(Qt::ShiftModifier);
        if (kind == Kind::Reposition)
        {
            d.offset = d.startOffset = offset();
            mDrag = std::move(d);
            return;
        }
        if (t.names.empty() && !(kind == Kind::Breakdown && d.all))
            return;
        g.evaluate();
        if (kind == Kind::Breakdown)
        {
            d.samples = sampleBreakdown(g, breakdownNodes(g, t.names, d.all), g.time());
            mDrag = std::move(d);
            return;
        }
        std::vector<NodeId> touched;
        auto touch = [&](NodeId id) {
            if (id && g.node(id) && g.node(id)->type().isA("transform") && std::find(touched.begin(), touched.end(), id) == touched.end())
                touched.push_back(id);
        };
        for (const std::string& n : t.names)
        {
            const NodeId id = g.find(n)->id();
            touch(id);
            touch(g.parent(id));
            for (NodeId c : g.children(id))
                touch(c);
        }
        if (t.foot)
            touch(t.foot);
        // A full-body move writes every joint of the character.
        if (d.tool == Tool::Move && !t.rig && settings().fullBody)
            for (const std::string& n : t.names)
                for (NodeId j : fullBodyJoints(g, g.find(n)->id()))
                    touch(j);
        // Posing a controller writes its chain's joint rotations, which a preview step must put back.
        if (t.chain)
        {
            touch(ikGoalOf(g, t.chain));
            for (NodeId j : lila::scene::rigel::posedJoints(g, t.chain))
                touch(j);
        }
        for (NodeId id : touched)
        {
            Saved s{id, {}};
            const Node& node = *g.node(id);
            for (int attr : {int(Transform::kTranslate), int(Transform::kRotate), int(Transform::kScale)})
                s.values.push_back({attr, g.value({id, attr, -1})});
            if (rig::isJoint(node))
                s.values.push_back({Joint::kJointOrient, g.value({id, Joint::kJointOrient, -1})});
            for (const char* ch : {"roll", "twist", "bank", "poleAngle"})
                if (const int a = attrOf(node, ch); a >= 0 && node.type().isA(kReverseFootType))
                    s.values.push_back({a, g.value({id, a, -1})});
            if (node.type().isA("ikHandle"))
                if (const int a = attrOf(node, "poleAngle"); a >= 0)
                    s.values.push_back({a, g.value({id, a, -1})});
            d.saved.push_back(std::move(s));
        }
        const NodeId first = g.find(t.names.front())->id();
        d.pivot = glm::dvec3(worldOf(g, first)[3]);
        d.axes = axesFor(g, first);
        mDrag = std::move(d);
    }

    void restore()
    {
        if (!mDrag)
            return;
        Graph& g = graph();
        quietly([&] {
            for (const Saved& s : mDrag->saved)
                if (g.node(s.id))
                    for (const auto& [attr, v] : s.values)
                        try
                        {
                            if (g.value({s.id, attr, -1}) != v)
                                g.setValue({s.id, attr, -1}, v);
                        }
                        catch (const std::exception&)
                        {
                        }
            for (const BreakdownSample& s : mDrag->samples)
                for (const auto& [attr, ch] : s.channels)
                    try
                    {
                        g.setValue({s.id, attr, -1}, s.current.at(attr));
                    }
                    catch (const std::exception&)
                    {
                    }
        });
    }

    /** Vertical travel since the press: up is positive. */
    double vertical() const { return -(mDrag->current.y() - mDrag->start.y()); }

    /** The drag so far as a command call; null name for no change. */
    std::pair<const char*, json> commandFor()
    {
        Drag& d = *mDrag;
        const Settings& s = settings();
        const double amount = vertical();
        const Target& t = d.target;
        json call = {{"args", namesJson(t.names)}};
        const int axis = isAxis(d.handle) ? axisOf(d.handle) : -1;
        const char* axisName[3] = {"X", "Y", "Z"};
        auto step = [&](double v, double st) { return s.discrete ? math::quantize(v, st) : v; };
        switch (d.kind)
        {
        case Kind::Pole:
        case Kind::PoleTurn:
        {
            const double by = step(amount * s.poleSpeed, s.rotateStep);
            call["args"] = {graph().node(t.chain)->path()};
            call["by"] = by;
            call["turn"] = d.kind == Kind::PoleTurn;
            d.readout = QString("Pole %1°%2").arg(by, 0, 'f', 1).arg(d.kind == Kind::PoleTurn ? "  with the controller" : "");
            return {"puppeteerPole", call};
        }
        case Kind::FootChannel:
        {
            static const char* names[] = {"roll", "twist", "bank"};
            const double by = step(amount * s.controllerSpeed, s.rotateStep);
            call["args"] = {graph().node(t.foot)->path()};
            call[names[axis]] = by;
            d.readout = QString("%1 %2°").arg(names[axis]).arg(by, 0, 'f', 1);
            return {"puppeteerFootChannels", call};
        }
        case Kind::Trackball:
        {
            const glm::dvec2 delta(d.current.x() - d.start.x(), d.current.y() - d.start.y());
            const auto [ax, angle] = math::trackball(delta, glm::transpose(glm::dmat3(mView.view)), s.trackballSpeed);
            if (angle == 0.0)
                return {"", call};
            call["axis"] = vecJson(ax);
            call["angle"] = angle;
            call["inherit"] = s.inherit;
            d.readout = QString("Free rotate %1°").arg(angle, 0, 'f', 1);
            return {"puppeteerRotate", call};
        }
        default: break;
        }
        switch (d.tool)
        {
        case Tool::Move:
        {
            glm::dvec3 by(0.0);
            if (axis >= 0)
            {
                const double dist = step(amount * cmPerPixelAlong(d.pivot, d.axes[axis]), s.moveStep);
                by = d.axes[axis] * dist;
                d.readout = QString("%1 %2 cm").arg(axisName[axis]).arg(dist, 0, 'f', 2);
            }
            else
            {
                by = screenDelta(d.start, d.current, d.pivot);
                if (s.discrete)
                    for (int k = 0; k < 3; ++k)
                        by[k] = math::quantize(by[k], s.moveStep);
                d.readout = QString("%1 cm").arg(glm::length(by), 0, 'f', 2);
            }
            call["by"] = vecJson(by);
            if (t.rig)
            {
                call["keepChildren"] = s.pivot;
                call["stickyAim"] = s.stickyAim;
                call["sticky2nd"] = s.sticky2nd;
                call["keepDirection"] = s.keepDirection;
            }
            else if (s.fullBody)
            {
                call["fullBody"] = true;
                call["autoPose"] = s.autoPose;
            }
            return {"puppeteerMove", call};
        }
        case Tool::Rotate:
        {
            const double speed = t.chain ? s.controllerSpeed : s.rotateSpeed;
            const double angle = step(amount * speed, s.rotateStep);
            const glm::dvec3 ax = axis >= 0 ? d.axes[axis] : -mView.viewDir(d.pivot);
            call["axis"] = vecJson(ax);
            call["angle"] = angle;
            call["inherit"] = s.inherit && !t.chain;
            d.readout = QString("%1%2°%3").arg(axis >= 0 ? QString(axisName[axis]) + " " : QString()).arg(angle, 0, 'f', 1).arg(s.inherit ? "  inherit" : "");
            return {"puppeteerRotate", call};
        }
        case Tool::Scale:
        {
            double f = 1.0 + amount * s.scaleSpeed;
            if (s.discrete)
                f = 1.0 + math::quantize(f - 1.0, s.scaleStep);
            f = std::max(f, 0.01);
            const glm::dvec3 by = axis < 0 ? glm::dvec3(f) : math::toonScale(axis, f, s.toon ? s.toonExponent : 0.0);
            call["by"] = vecJson(by);
            call["compensate"] = s.toon;
            d.readout = QString("%1x%2%3").arg(axis >= 0 ? QString(axisName[axis]) + " " : QString()).arg(f, 0, 'f', 3).arg(s.toon ? "  toon" : "");
            return {"puppeteerScale", call};
        }
        }
        return {"", call};
    }

    /**
     * Centimetres per pixel of drag along a world axis: the joint moves as far on screen as the cursor, along the axis's
     * own projection. An axis pointing at the camera is capped at four times the screen-plane rate.
     */
    double cmPerPixelAlong(const glm::dvec3& pivot, const glm::dvec3& axis) const
    {
        const double upp = mView.unitsPerPixel(pivot);
        const auto a = mView.project(pivot), b = mView.project(pivot + glm::normalize(axis) * upp * 10.0);
        if (!a || !b)
            return upp;
        const double px = std::hypot(b->x() - a->x(), b->y() - a->y());
        return upp * 10.0 / std::max(px, 2.5);
    }

    /** World offset between two cursor positions on the plane through pivot facing the view. */
    glm::dvec3 screenDelta(QPointF a, QPointF b, const glm::dvec3& pivot) const
    {
        const glm::dvec3 n = mView.viewDir(pivot);
        auto onPlane = [&](QPointF px) {
            glm::dvec3 o, dir;
            mView.ray(px, o, dir);
            const double den = glm::dot(dir, n);
            return std::abs(den) < 1e-12 ? pivot : o + dir * (glm::dot(pivot - o, n) / den);
        };
        // The drag starts on the Spinner, away from the joint, so only the cursor's travel counts.
        return onPlane(b) - onPlane(a);
    }

    void apply(bool commit)
    {
        Drag& d = *mDrag;
        Graph& g = graph();
        if (d.kind == Kind::Reposition)
        {
            d.offset = d.startOffset + (d.current - d.start);
            if (commit)
            {
                // Stored where center() clamped it, so a drag past the edge doesn't leave it out of reach.
                settings().offsets[mView.camera] = center() - QPointF(mView.width * 0.5, mView.height * 0.5);
                changed();
            }
            return;
        }
        if (d.kind == Kind::Breakdown)
        {
            const Settings& s = settings();
            d.weight = math::breakdownWeight(d.current.x() - d.start.x(), s.breakdownDeadZone, s.breakdownFull);
            d.readout = d.weight < 0.0 ? QString("Previous key %1%").arg(int(std::round(-d.weight * 100.0)))
                                       : QString("Next key %1%").arg(int(std::round(d.weight * 100.0)));
            if (commit)
            {
                restore();
                if (d.weight != 0.0)
                    runCommand("puppeteerBreakdown", {{"args", namesJson(d.target.names)}, {"weight", d.weight}, {"all", d.all}});
                return;
            }
            quietly([&] {
                for (const BreakdownSample& s : d.samples)
                    for (const auto& [attr, ch] : s.channels)
                        try
                        {
                            g.setValue({s.id, attr, -1}, breakdownValue(g, s, attr, d.weight));
                        }
                        catch (const std::exception&)
                        {
                        }
            });
            return;
        }
        auto [name, call] = commandFor();
        restore();
        if (!*name)
            return;
        if (commit)
        {
            runCommand(name, call);
            return;
        }
        // A failing preview says why once per drag rather than doing nothing silently.
        if (std::string error = quietly([&] { ops::execute(name, call, CallSource::Script); }); !error.empty() && !d.warned)
        {
            d.warned = true;
            log::write(LogLevel::Warning, "Spinner: " + error);
        }
    }

    ViewInfo mView;
    Handle mHover = Handle::None;
    std::optional<Drag> mDrag;
    mutable std::string mLastMode;
};

void registerIcons(ext::Context& ctx)
{
    const std::pair<const char*, const char*> icons[] = {
        {"puppeteer.move", "arrows-move"},         {"puppeteer.rotate", "rotate-clockwise"}, {"puppeteer.inherit", "rotate-2"},
        {"puppeteer.scale", "arrows-maximize"},    {"puppeteer.toon", "resize"},             {"puppeteer.settings", "settings"},
        {"puppeteer.help", "help"},                {"puppeteer.world", "world"},             {"puppeteer.local", "cube"},
        {"puppeteer.screen", "device-desktop"},    {"puppeteer.pivot", "focus-centered"},    {"puppeteer.discrete", "grid-dots"},
        {"puppeteer.sticky", "target-arrow"},      {"puppeteer.sticky2nd", "vector-bezier"}, {"puppeteer.keepDirection", "direction"},
        {"puppeteer.foot", "shoe"},                {"puppeteer.footAlign", "line"},          {"puppeteer.pole", "target"},
        {"puppeteer.proxy", "puzzle"},
    };
    for (const auto& [id, tabler] : icons)
        overlayIcons::add(ctx, id, {.tabler = tabler});
}

void registerSpinnerActions(ext::Context& ctx)
{
    const char* cat = "Puppeteer Spinner";
    auto flag = [&](const char* id, const char* label, bool Settings::* member, const char* doc) {
        actions::add(ctx, {.id = id, .label = label, .category = cat,
                           .run =
                               [member] {
                                   settings().*member = !(settings().*member);
                                   changed();
                               },
                           .doc = doc});
    };
    actions::add(ctx, {.id = "puppeteer.spinner.move", .label = "Spinner Move", .category = cat, .run = [] { setTool(Tool::Move); },
                       .doc = "The Spinner moves the selection."});
    actions::add(ctx, {.id = "puppeteer.spinner.rotate", .label = "Spinner Rotate", .category = cat,
                       .run = [] { toolOrAlternative(Tool::Rotate); },
                       .doc = "The Spinner rotates the selection; again to toggle Inherit Rotate."});
    actions::add(ctx, {.id = "puppeteer.spinner.scale", .label = "Spinner Scale", .category = cat,
                       .run = [] { toolOrAlternative(Tool::Scale); },
                       .doc = "The Spinner scales the selection; again to toggle Toon Scale."});
    actions::add(ctx, {.id = "puppeteer.spinner.inheritRotate", .label = "Inherit Rotate", .category = cat,
                       .run =
                           [] {
                               settings().inherit = !settings().inherit;
                               setTool(Tool::Rotate);
                           },
                       .doc = "Rotate with the children keeping their world orientation, or back to plain rotate."});
    actions::add(ctx, {.id = "puppeteer.spinner.toonScale", .label = "Toon Scale", .category = cat,
                       .run =
                           [] {
                               settings().toon = !settings().toon;
                               setTool(Tool::Scale);
                           },
                       .doc = "Scale one axis with the other two compensating (squash and stretch), or back to plain scale."});
    for (auto [id, label, space] : {std::tuple{"puppeteer.spinner.local", "Spinner Local Space", Space::Local},
                                    {"puppeteer.spinner.world", "Spinner World Space", Space::World},
                                    {"puppeteer.spinner.screen", "Spinner Screen Space", Space::Screen}})
        actions::add(ctx, {.id = id, .label = label, .category = cat,
                           .run =
                               [space] {
                                   settings().space = space;
                                   changed();
                               },
                           .doc = "The axes the Spinner's zones drag along."});
    actions::add(ctx, {.id = "puppeteer.spinner.cycleSpace", .label = "Cycle Spinner Space", .category = cat,
                       .run =
                           [] {
                               settings().space = Space((int(settings().space) + 1) % 3);
                               changed();
                           },
                       .doc = "The Spinner's axes: local, world, screen."});
    flag("puppeteer.spinner.discrete", "Discrete Steps", &Settings::discrete, "Spinner drags move, turn and scale in fixed steps.");
    flag("puppeteer.spinner.stickyAim", "Sticky Aim", &Settings::stickyAim, "Rigging: orients follow the bones as joints move.");
    flag("puppeteer.spinner.pivot", "Pivot", &Settings::pivot, "Rigging: the Spinner moves joints without their children.");
    flag("puppeteer.spinner.fullBody", "Full Body Move", &Settings::fullBody,
         "Animating: moving a joint poses the whole body, holding pinned joints and locked hands and feet.");
    flag("puppeteer.spinner.autoPose", "Auto Pose", &Settings::autoPose,
         "Full-body moves let the auto-poser place the rest of the body, as a person would move to reach.");
}
} // namespace

void registerSpinner(ext::Context& ctx)
{
    registerIcons(ctx);
    registerSpinnerActions(ctx);
    gizmos::add(ctx, {.id = kSpinnerGizmo, .label = "Spinner", .create = [] { return std::make_unique<SpinnerGizmo>(); }});
}
} // namespace puppeteer
