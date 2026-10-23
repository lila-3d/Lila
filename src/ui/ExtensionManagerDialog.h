#pragma once

#include "core/Export.h"

#include <QDialog>

class QLabel;
class QTableWidget;

namespace lila::ui
{
/**
 * Maya's Plug-in Manager: every extension on the search paths with its version, vendor and SDK, whether it is loaded
 * and whether it loads at startup, and why it failed to load. Load, unload and auto load act at once.
 */
class LILA_UI_API ExtensionManagerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExtensionManagerDialog(QWidget* parent = nullptr);
    ~ExtensionManagerDialog() override;
    static void open(QWidget* parent);

    /** Re-reads the search paths. */
    void refresh();

private:
    void setLoaded(const std::string& name, bool on);
    void browse();
    void showInfo(const std::string& name);

    QTableWidget* mTable = nullptr;
    QLabel* mPaths = nullptr;
    bool mFilling = false;
    uint64_t mLoadedSub = 0;
    uint64_t mUnloadedSub = 0;
};
} // namespace lila::ui
