#pragma once

#include "core/Export.h"

#include <QWidget>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace lila::ui
{
/**
 * The file dialog's model preview: FBX, OBJ and Lila scene files under a size limit load on a worker thread and draw as a shaded
 * turntable, software rasterized so it needs no GPU context. Drag orbits, the wheel zooms, double-click resets.
 */
class LILA_UI_API FilePreview : public QWidget
{
public:
    /** Largest file previewed, in MB. */
    static constexpr const char* kMaxMbPref = "/fileDialog/previewMaxMB";

    explicit FilePreview(QWidget* parent = nullptr);
    ~FilePreview() override;

    static bool canPreview(const QString& path);
    /** Loads `path` if it can be previewed and isn't too large; returns whether there is anything to show. */
    bool setFile(const QString& path);
    QString file() const { return mFile; }
    size_t triangleCount() const { return mCorners.size() / 3; }
    /** True while the worker is still reading the file. */
    bool loading() const { return mLoading; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void loaded(std::vector<glm::vec3> corners);
    void resetView();
    void rasterize();

    QString mFile;
    QString mMessage;
    std::vector<glm::vec3> mCorners;
    glm::vec3 mCenter{0.0f};
    float mRadius = 1.0f;
    float mYaw = 0.0f, mPitch = 0.0f, mZoom = 1.0f;
    QPoint mLastMouse;
    QImage mImage;
    bool mDirty = true;
    bool mLoading = false;
    /** Bumped per load, so a slow worker's result for an earlier file is dropped. */
    int mGeneration = 0;
    std::shared_ptr<bool> mAlive;
};
} // namespace lila::ui
