#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <QFrame>

#include <string>

class QLabel;
class QLineEdit;
class QListWidget;

namespace lila::ui
{
/**
 * Connect many plugs at once between two objects: outputs of the source on the left, inputs of the target on the right,
 * each with a filter. Click a left plug to light up the inputs it fits, then click one to wire it; Match by Name wires every
 * same-named pair. Replaces Maya's Connection Editor; stays open while wiring and closes on Esc.
 */
class LILA_UI_API ConnectPopover : public QFrame
{
    Q_OBJECT

public:
    ConnectPopover(scene::NodeId source, scene::NodeId target, QWidget* parent = nullptr);
    ~ConnectPopover() override;

    /** Connects the left list's current plug to the named right plug. */
    bool wire(const std::string& from, const std::string& to);
    /** connectByName between the two. Returns how many were wired. */
    int matchByName();
    void swap();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refill();
    void markCompatible();

    scene::NodeId mSource;
    scene::NodeId mTarget;
    QLabel* mTitle = nullptr;
    QLineEdit* mLeftFilter = nullptr;
    QLineEdit* mRightFilter = nullptr;
    QListWidget* mLeft = nullptr;
    QListWidget* mRight = nullptr;
    QLabel* mStatus = nullptr;
    int mListener = 0;
};

namespace connectPopover
{
/** Opens on the first two selected objects (the first is the source). Null, with a log message, unless two are selected. */
LILA_UI_API ConnectPopover* showForSelection(QWidget* anchor);
} // namespace connectPopover

/** This stage's actions, menus and panels: Connect Attributes, the component spreadsheet, Relationships. */
LILA_UI_API void registerAttributeUi();
} // namespace lila::ui
