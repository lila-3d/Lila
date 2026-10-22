#pragma once

#include "render/FrameState.h"

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <memory>
#include <vector>

namespace lila::ui
{
/**
 * Builds a viewport's 2D overlay: thick anti-aliased lines, fills and text, in widget (logical) pixels. Converted to
 * physical pixels for the renderer, so callers never think about DPI.
 */
class OverlayPainter
{
public:
    OverlayPainter(std::vector<render::OverlayVertex>& out, double dpr) : mOut(out), mDpr(dpr) {}

    void line(QPointF a, QPointF b, float width, const glm::vec4& color);
    void dashedLine(QPointF a, QPointF b, float width, float dash, const glm::vec4& color);
    void polyline(const std::vector<QPointF>& pts, float width, const glm::vec4& color, bool closed = false);
    /** A polyline broken into dashes of `dash` pixels by arc length, so curves dash evenly however finely sampled. */
    void dashedPolyline(const std::vector<QPointF>& pts, float width, float dash, const glm::vec4& color, bool closed = false);
    void triangle(QPointF a, QPointF b, QPointF c, const glm::vec4& color);
    /** Convex polygon fill. */
    void fill(const std::vector<QPointF>& pts, const glm::vec4& color);
    void rect(QRectF r, const glm::vec4& color);
    void disc(QPointF center, float radius, const glm::vec4& color, int segments = 20);

    enum Align
    {
        Left,
        Center,
        Right,
    };
    /** Text with its baseline at `at`. Returns the advance width. */
    float text(QPointF at, const QString& s, const glm::vec4& color, Align align = Left);
    /** Text over a dark rounded plate, for readouts that must stay legible on any background. */
    void label(QPointF at, const QString& s, const glm::vec4& color = {0.95f, 0.95f, 0.95f, 1.0f});
    static float textWidth(const QString& s);
    /** A Tabler icon from the atlas's set (kOverlayIcons in Overlay.cpp) filling r; false when it is not in the set. */
    bool icon(QRectF r, const char* name, const glm::vec4& color);

    /** While on, lines, triangles and text get an under-stroke that contrasts with their own color. */
    void setShadow(bool on) { mShadow = on; }

    /**
     * Which parts of the background are bright: 1 or 0 per cell of cols x rows over a view of the given logical size,
     * top row first, from classifyBackdrop(). Without it the backdrop counts as dark.
     */
    void setBackdrop(std::vector<float> bright, int cols, int rows, QSizeF view);
    /**
     * Turns render::backdropLuminance() cells into setBackdrop()'s bright flags, updating state in place. Around the
     * switch point (where black and white text contrast equally) a cell keeps its previous answer, so orbiting across it
     * never flickers.
     */
    static void classifyBackdrop(const std::vector<float>& luminance, std::vector<float>& state);
    /** Whether most of the backdrop under r is bright; an empty r means the whole view. */
    bool brightBackdrop(QRectF r = {}) const;
    /**
     * A chrome gray that reads on the backdrop under r. shade is the gray as it would be on a dark background (0.85
     * bright, 0.45 dim); over a bright backdrop it comes back as a dark gray, the dim ones a little lighter.
     */
    glm::vec4 ink(float shade, float alpha = 1.0f, QRectF r = {}) const;

private:
    void vertex(QPointF p, glm::vec2 uv, const glm::vec4& color);

    /** The under-stroke for a stroke of color c at alpha a: white under dark grays, black under everything else. */
    static glm::vec4 shadowFor(const glm::vec4& c, float a);

    std::vector<render::OverlayVertex>& mOut;
    double mDpr;
    bool mShadow = false;
    std::vector<float> mBackdrop;
    int mBackdropCols = 0, mBackdropRows = 0;
    QSizeF mBackdropView;
    float mBackdropMean = 0.0f;
};

/** The glyph atlas every viewport shares. Built on first use from the UI font. */
std::shared_ptr<const render::OverlayAtlas> overlayAtlas();
} // namespace lila::ui
