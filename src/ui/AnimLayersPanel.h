#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>

class QTreeWidget;
class QTreeWidgetItem;

namespace lila::ui
{
inline constexpr const char* kAnimLayersDock = "animLayers";

/**
 * The animation layer stack, top layer first and BaseAnimation last. Each row has M (mute), S (solo), L (lock), the
 * mode (A additive, O override), the weight and the name; clicking the name makes that layer the one keys go to.
 * The toolbar creates, adds and removes members, reorders, merges and deletes.
 */
class LILA_UI_API AnimLayersPanel : public QWidget
{
public:
    explicit AnimLayersPanel(QWidget* parent = nullptr);
    ~AnimLayersPanel() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    enum Column
    {
        kMute,
        kSolo,
        kLock,
        kMode,
        kWeight,
        kName,
    };

    void scheduleRefresh();
    void refresh();
    void clicked(QTreeWidgetItem* item, int column);
    /** The row the toolbar acts on; empty for BaseAnimation or nothing. */
    std::string currentLayer() const;

    QTreeWidget* mTree = nullptr;
    int mGraphListener = 0;
    bool mPending = false;
    bool mFilling = false;
};

/** Registers the Anim Layers panel and its window.animLayers toggle with the panel registry. */
LILA_UI_API void registerAnimLayersPanel();
} // namespace lila::ui
