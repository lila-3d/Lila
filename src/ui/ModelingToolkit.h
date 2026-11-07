#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>
#include <vector>

class QTimer;
class QToolButton;

namespace lila::ui
{
/** Dock object name for the Modeling Toolkit. */
inline constexpr const char* kModelingToolkitDock = "modelingToolkit";

/**
 * The Modeling Toolkit: the daily edit tools as tiles, then labelled groups of mesh, component and selection commands
 * with the rest in overflow menus, then collapsed sections for normals, shaping, retopology, auto bevel, curves to mesh
 * and the last operation's live settings. Component modes, soft select and symmetry live in the viewport header.
 */
class LILA_UI_API ModelingToolkit : public QWidget
{
    Q_OBJECT

public:
    explicit ModelingToolkit(QWidget* parent = nullptr);
    ~ModelingToolkit() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    /** A tile for a registered action, or null when it isn't one. */
    QToolButton* tile(const std::string& id, const char* iconName, const QString& label, QWidget* parent);
    /** A labelled push button for a registered action, or null when it isn't one. */
    QToolButton* push(const std::string& id, const QString& label, QWidget* parent);
    QToolButton* track(QToolButton* b, const std::string& id);
    void refreshState();

    QTimer* mRefresh = nullptr;
    std::vector<QToolButton*> mButtons;
    int mGraphListener = 0;
    int mPrefListener = 0;
};

/** Registers "window.modelingToolkit", which shows or hides the dock named kModelingToolkitDock. */
LILA_UI_API void registerModelingToolkitActions();
} // namespace lila::ui
