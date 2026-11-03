#pragma once

#include "core/Export.h"

#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;

namespace lila::ui
{
inline constexpr const char* kOffsetsDock = "offsets";

/**
 * Polish offsets (scene/Offsets.h), top first: mute, weight, name, the joints and the range. The toolbar creates one
 * from the selected joints (with its option box), selects an offset's joints, bakes it down and deletes it. Clicking
 * the name makes it the layer keys go to, so posing adjusts it.
 */
class LILA_UI_API OffsetsPanel : public QWidget
{
public:
    explicit OffsetsPanel(QWidget* parent = nullptr);
    ~OffsetsPanel() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    enum Column
    {
        kMute,
        kWeight,
        kName,
        kInfo,
    };

    void scheduleRefresh();
    void refresh();
    std::string currentOffset() const;

    QTreeWidget* mTree = nullptr;
    int mGraphListener = 0;
    bool mPending = false;
};

/** The Offsets panel, its window.offsets toggle and the anim.createOffset / bakeOffset / deleteOffset actions. */
LILA_UI_API void registerOffsetsPanel();
} // namespace lila::ui
