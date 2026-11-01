#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"
#include "core/Registry.h"

#include "scene/Node.h"
#include "ui/Overlay.h"
#include "ui/ViewInfo.h"

#include <QPointF>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

/**
 * Handles on selected nodes of one type, for their own settings (a light's cone, range and colour), drawn and dragged in
 * every viewport alongside the move/rotate/scale manipulator in object mode. They take the mouse before the manipulator,
 * so keep them small and off its axes. Not shown while a viewport tool is active.
 */
namespace lila::ui::nodeHandles
{
struct Context
{
    const ViewInfo& view;
    const scene::Node& node;
    glm::dmat4 world{1.0};
};

/**
 * A drag under way, made by Provider::begin. Edit the scene with undo off while it moves, and on release(true) put the
 * start values back and run one command, so undo and the script echo see a single edit.
 */
class Drag
{
public:
    virtual ~Drag() = default;
    virtual void move(QPointF px, const ViewInfo& view, Qt::KeyboardModifiers mods) = 0;
    /** moved is false for a click: the cursor never left the press by more than a few pixels. */
    virtual void release(bool moved) = 0;
    /** Puts back what the drag changed. */
    virtual void cancel() = 0;
    /** Drawn by the cursor while dragging ("Range 250 cm"); empty for none. */
    virtual QString readout() const { return {}; }
};

struct Provider
{
    std::string id;
    /** The node type it handles, and the types derived from it. */
    std::string nodeType;
    /** One selected node's handles. hovered is the handle under the cursor, active the one dragged, -1 for none. */
    std::function<void(OverlayPainter& p, const Context& c, int hovered, int active)> draw;
    /** The handle at px, -1 for none. */
    std::function<int(QPointF px, const Context& c)> pick;
    /** Starts dragging handle; null takes the press without a drag. */
    std::function<std::unique_ptr<Drag>(int handle, QPointF px, const Context& c)> begin;
};

LILA_UI_API Registry<Provider>& registry();
/** Throws std::invalid_argument when the id is taken. */
LILA_UI_API const Provider& add(Provider p);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the id is taken. */
LILA_UI_API const Provider* add(ext::Context& ctx, Provider p);
LILA_UI_API bool remove(std::string_view id);
/** The provider for nodes of type t or its nearest base type; null when none. */
LILA_UI_API const Provider* forType(const scene::NodeType& t);

/** One viewport's hover and drag state. Removing the provider of a drag in progress cancels it. */
class LILA_UI_API Driver
{
public:
    Driver();
    ~Driver();
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    /** Updates the hovered handle; true when it changed. */
    bool hover(QPointF px, const ViewInfo& view);
    /** True when the press landed on a handle. */
    bool press(QPointF px, const ViewInfo& view);
    void drag(QPointF px, const ViewInfo& view, Qt::KeyboardModifiers mods);
    void release();
    void cancel();
    bool dragging() const { return mDrag != nullptr; }
    bool hovering() const { return mHover.has_value(); }
    void draw(OverlayPainter& p, const ViewInfo& view);

private:
    struct Hit
    {
        scene::NodeId node = scene::kNoNode;
        std::string provider;
        int handle = -1;
        bool operator==(const Hit&) const = default;
    };
    std::optional<Hit> pick(QPointF px, const ViewInfo& view) const;

    std::optional<Hit> mHover;
    std::optional<Hit> mActive;
    std::unique_ptr<Drag> mDrag;
    QPointF mPress;
    QPointF mLast;
    bool mMoved = false;
};
} // namespace lila::ui::nodeHandles
