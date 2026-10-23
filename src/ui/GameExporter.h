#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QRadioButton;

namespace lila::ui
{
/**
 * Maya's File > Game Exporter, Model tab: export all, the selection or chosen object sets to FBX or OBJ, as one file,
 * a file per set or a file per object, with the FBX options. Export runs gameExport; presets save to the prefs or the
 * scene. The last settings used are remembered in the "gameExporter" prefs.
 */
class LILA_UI_API GameExporter : public QDialog
{
    Q_OBJECT

public:
    explicit GameExporter(QWidget* parent = nullptr);
    ~GameExporter() override;

    static void open(QWidget* parent);
    /** "file.gameExporter". */
    static void registerActions(QWidget* parent);

private:
    nlohmann::json settings() const;
    void apply(const nlohmann::json& settings);
    void refreshSets();
    void refreshPresets();
    void updatePreview();
    void scheduleRefresh();
    void exportNow();
    void savePreset();
    void deletePreset();

    int mListener = 0;
    bool mApplying = false;
    bool mQueued = false;
    QComboBox* mPreset = nullptr;
    QRadioButton* mAll = nullptr;
    QRadioButton* mSelection = nullptr;
    QRadioButton* mSets = nullptr;
    QListWidget* mSetList = nullptr;
    QComboBox* mSeparate = nullptr;
    QCheckBox* mMoveToOrigin = nullptr;
    QComboBox* mFileType = nullptr;
    QComboBox* mProfile = nullptr;
    QComboBox* mUnits = nullptr;
    QCheckBox* mTriangulate = nullptr;
    QCheckBox* mSmoothing = nullptr;
    QCheckBox* mTangents = nullptr;
    QCheckBox* mEmbedMedia = nullptr;
    QLineEdit* mPath = nullptr;
    QLineEdit* mFilename = nullptr;
    QLabel* mPreview = nullptr;
};
} // namespace lila::ui
