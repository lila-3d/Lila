#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>

class QTreeWidget;
class QTreeWidgetItem;

namespace lila::ui
{
inline constexpr const char* kDisplayLayersDock = "displayLayers";

/**
 * Maya's Display Layer editor: one row per layer with V (visibility), P (hide on playback), the display type (blank,
 * T template, R reference), a colour swatch and the name. Clicking the name selects the layer's objects; double-click
 * renames; the right-click menu and the toolbar add, remove, reorder and delete.
 */
class LILA_UI_API DisplayLayersPanel : public QWidget
{
public:
    explicit DisplayLayersPanel(QWidget* parent = nullptr);
    ~DisplayLayersPanel() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    void scheduleRefresh();
    void refresh();
    void clicked(QTreeWidgetItem* item, int column);
    void contextMenu(const QPoint& at);
    void pickColor(const std::string& layer);
    void rename(QTreeWidgetItem* item);
    /** The row the toolbar acts on: the current one, else the top. Empty without layers. */
    std::string currentLayer() const;

    QTreeWidget* mTree = nullptr;
    int mGraphListener = 0;
    bool mPending = false;
    bool mFilling = false;
    bool mEditing = false; // a rename is open; a rebuild would end it
};

/** layers.create, layers.createFromSelected, layers.addSelected, layers.removeSelected. */
LILA_UI_API void registerDisplayLayerActions();
} // namespace lila::ui
