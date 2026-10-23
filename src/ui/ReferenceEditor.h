#pragma once

#include "core/Export.h"

#include <QWidget>

class QFileSystemWatcher;
class QTreeWidget;

namespace lila::ui
{
/** Maya's Reference Editor: every reference with its file, state and edit count; load, unload, reload, remove, import. */
class LILA_UI_API ReferenceEditor : public QWidget
{
    Q_OBJECT

public:
    explicit ReferenceEditor(QWidget* parent = nullptr);
    ~ReferenceEditor() override;

    void refresh();
    /** File dialog, then createReference. */
    static void createReferenceDialog(QWidget* parent);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void runOnSelected(const char* command);
    /** Coalesces scene changes into one refresh; a hidden editor refreshes when it is shown. */
    void scheduleRefresh();
    void watchFiles();

    QTreeWidget* mTree = nullptr;
    QFileSystemWatcher* mWatcher = nullptr;
    QStringList mChanged;
    int mListener = 0;
    bool mQueued = false;
    bool mStale = false;
};
} // namespace lila::ui
