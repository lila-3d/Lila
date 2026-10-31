#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace lila::ui
{
/** The last uvPack options used from the UV editor, as uvPack flags; the uv.pack actions reuse them. */
LILA_UI_API nlohmann::json uvPackOptions();
LILA_UI_API void setUvPackOptions(const nlohmann::json& options);

/** The UV editor's Pack options in sections: which islands and how they separate, fit, space, search. */
class LILA_UI_API UvPackPanel : public QWidget
{
    Q_OBJECT

public:
    explicit UvPackPanel(QWidget* parent = nullptr);

    /** uvPack flags for the controls, without resolution and padding (the editor owns those). */
    nlohmann::json options() const;
    /** Shows a uvPack result: density, texel density, stacks, time. */
    void showResult(const nlohmann::json& result);

    std::function<void(bool)> onShowStacks;

private:
    void save();
    void load(const nlohmann::json& o);
    /** Greys out what the current choices make irrelevant. */
    void updateEnabled();

    QComboBox* mRotation = nullptr;
    QCheckBox* mFlip = nullptr;
    QCheckBox* mOrient = nullptr;
    QComboBox* mScale = nullptr;
    QDoubleSpinBox* mDensity = nullptr;
    QCheckBox* mStack = nullptr;
    QCheckBox* mStackMirrored = nullptr;
    QComboBox* mSeparate = nullptr;
    QComboBox* mInto = nullptr;
    QLineEdit* mGroupAttr = nullptr;
    QSpinBox* mTilesU = nullptr;
    QSpinBox* mTilesV = nullptr;
    QDoubleSpinBox* mRegion[4] = {};
    QDoubleSpinBox* mMargin = nullptr;
    QSpinBox* mMips = nullptr;
    QComboBox* mKeep = nullptr;
    QCheckBox* mFixPinned = nullptr;
    QCheckBox* mKeepTile = nullptr;
    QComboBox* mEffort = nullptr;
    QDoubleSpinBox* mTimeLimit = nullptr;
    QCheckBox* mShowStacks = nullptr;
    QLabel* mResult = nullptr;
    bool mLoading = false;
};

/** Registers the uv.pack* actions (pack with the last options, around the selection, stacked, select stacks). */
LILA_UI_API void registerUvPackActions();
} // namespace lila::ui
