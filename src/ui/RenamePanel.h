#pragma once

#include "core/Export.h"

#include <QWidget>

#include <nlohmann/json.hpp>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QTreeWidget;

namespace lila::ui
{
inline constexpr const char* kRenameDock = "rename";

/**
 * Batch rename (the batchRename command) over the selection or the selected hierarchies: a chain name with padded
 * numbers, find/replace with optional regex, strip, case, prefix and suffix, with every old and new name previewed live.
 */
class LILA_UI_API RenamePanel : public QWidget
{
public:
    explicit RenamePanel(QWidget* parent = nullptr);
    ~RenamePanel() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    nlohmann::json call(bool preview) const;
    void scheduleRefresh();
    void refresh();
    void apply();
    void reset();

    QComboBox* mScope = nullptr;
    QLineEdit* mBase = nullptr;
    QLineEdit* mSeparator = nullptr;
    QSpinBox* mPadding = nullptr;
    QSpinBox* mStart = nullptr;
    QSpinBox* mStep = nullptr;
    QLineEdit* mFind = nullptr;
    QLineEdit* mReplace = nullptr;
    QCheckBox* mRegex = nullptr;
    QCheckBox* mMatchCase = nullptr;
    QSpinBox* mStripStart = nullptr;
    QSpinBox* mStripEnd = nullptr;
    QCheckBox* mStripDigits = nullptr;
    QComboBox* mCase = nullptr;
    QLineEdit* mPrefix = nullptr;
    QLineEdit* mSuffix = nullptr;
    QTreeWidget* mPreview = nullptr;
    int mGraphListener = 0;
    bool mPending = false;
};

/** The Rename panel, its window.rename toggle and edit.batchRename, which opens it. */
LILA_UI_API void registerRenamePanel();
} // namespace lila::ui
