#pragma once

#include "scene/Plug.h"

#include <QWidget>

class QLineEdit;
class QTreeWidget;

namespace lila::ui
{
class CurveView;

/**
 * Maya's graph editor: the selection's animated channels on the left, their curves on the right. Alt+MMB pans,
 * Alt+RMB zooms (drag sideways for time, up and down for value), the wheel zooms both. LMB selects keys (click or
 * box; Shift adds, Ctrl removes) and drags them, Shift constrains to one axis; MMB drags the selected keys too.
 * Selected keys show tangent handles. Every edit runs through commands, so it undoes.
 */
class GraphEditor : public QWidget
{
public:
    explicit GraphEditor(QWidget* parent = nullptr);
    ~GraphEditor() override;

    /** The editor hotkeys act on: the last one focused. */
    static GraphEditor* instance();

    /** Fits the view to the selected keys, or to every shown curve. */
    void frame(bool selectedOnly);
    void deleteSelectedKeys();

protected:
    void focusInEvent(QFocusEvent* e) override;

private:
    friend class CurveView;

    void scheduleRebuild();
    void rebuild();
    void reloadCurves();
    void applyVisibility();
    void applyTangent(const char* type);
    void applyInfinity(bool pre, const char* mode);
    void refreshKeyFields();
    void editKeyField(bool time);

    QTreeWidget* mChannels = nullptr;
    CurveView* mView = nullptr;
    QLineEdit* mKeyTime = nullptr;
    QLineEdit* mKeyValue = nullptr;
    int mListener = 0;
    bool mQueued = false;
    bool mCurvesQueued = false;
};
} // namespace lila::ui
