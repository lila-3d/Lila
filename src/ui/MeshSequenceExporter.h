#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;

namespace lila::ui
{
/**
 * File > Export Mesh Sequence: the selected animated meshes frame by frame, as a file per frame (FBX, OBJ) or one
 * Alembic file, through exportMeshSequence. Send to Unreal maps them as a meshSequence (static mesh per frame, optionally
 * feeding a Niagara mesh renderer) or geometryCache with the same settings. The last settings are kept in the
 * "meshSequence" prefs.
 */
class LILA_UI_API MeshSequenceExporter : public QDialog
{
    Q_OBJECT

public:
    explicit MeshSequenceExporter(QWidget* parent = nullptr);
    ~MeshSequenceExporter() override;

    static void open(QWidget* parent);
    /** "file.exportMeshSequence". */
    static void registerActions(QWidget* parent);

private:
    nlohmann::json settings() const;
    void apply(const nlohmann::json& s);
    nlohmann::json call(bool dryRun) const;
    void updatePreview();
    void scheduleRefresh();
    void exportNow();
    void sendToUnreal();

    int mListener = 0;
    bool mApplying = false;
    bool mQueued = false;
    QLabel* mObjects = nullptr;
    QRadioButton* mPlayback = nullptr;
    QRadioButton* mCustom = nullptr;
    QDoubleSpinBox* mStart = nullptr;
    QDoubleSpinBox* mEnd = nullptr;
    QDoubleSpinBox* mStep = nullptr;
    QComboBox* mFormat = nullptr;
    QLineEdit* mFolder = nullptr;
    QLineEdit* mName = nullptr;
    QComboBox* mNumbering = nullptr;
    QSpinBox* mPadding = nullptr;
    QDoubleSpinBox* mScale = nullptr;
    QComboBox* mProfile = nullptr;
    QComboBox* mSpace = nullptr;
    QComboBox* mRecenter = nullptr;
    QCheckBox* mModifiers = nullptr;
    QCheckBox* mNormals = nullptr;
    QCheckBox* mUvs = nullptr;
    QCheckBox* mColors = nullptr;
    QCheckBox* mTriangulate = nullptr;
    QLineEdit* mContentPath = nullptr;
    QLineEdit* mNiagara = nullptr;
    QLabel* mPreview = nullptr;
};
} // namespace lila::ui
