#pragma once

#include "core/Export.h"

#include <QWidget>

#include <functional>
#include <optional>
#include <string>

class QMenu;

namespace lila::ui
{
class ViewportHeader;
class ViewportWidget;

/**
 * Checked state of a stateful display action ("viewport.toggleGrid", "viewport.view.top", ...) as seen from view, so
 * menus and toolbars agree; nullopt for actions that are not toggles. view may be null for the global ones.
 */
LILA_UI_API std::optional<bool> viewportActionState(const std::string& id, const ViewportWidget* view);

/**
 * Maya's camera menu for a view: the built-in views, every camera node in the scene (checked when the view looks
 * through it), Look Through Selected and Create Camera from View. `activate` runs first on each pick, so actions that
 * target the active view act on this one.
 */
LILA_UI_API void fillCameraMenu(QMenu* menu, ViewportWidget* view, std::function<void()> activate = {});

/** Fills menu with the built-in HDRIs, the procedural studio and Browse..., checking the current environment. */
LILA_UI_API void fillEnvironmentMenu(QMenu* menu);

/** One viewport with its header row above it, as a cell of the viewport layout. */
class LILA_UI_API ViewportPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportPanel(QWidget* parent = nullptr);
    ~ViewportPanel() override;

    ViewportWidget* view() const { return mView; }

private:
    ViewportWidget* mView;
    ViewportHeader* mHeader;
    int mPrefListener = 0;
};
} // namespace lila::ui
