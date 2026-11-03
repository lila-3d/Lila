#include "ui/Overlay.h"

#include "core/Log.h"
#include "core/Registry.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <map>
#include <unordered_map>

namespace lila::ui
{
namespace
{
struct Glyph
{
    QRectF uv;       // normalized atlas rect
    QRectF box;      // logical px, relative to the pen position on the baseline
    float advance = 0.0f;
};

/** The Tabler icons Lila's own cursor badges draw. */
constexpr const char* kBuiltInIcons[] = {"grid-pattern", "line", "point", "angle", "target", "circle-dotted", "feather", "perspective"};
constexpr double kIconPx = 16.0; // logical size they are baked for
} // namespace

struct OverlayAtlasData
{
    std::shared_ptr<render::OverlayAtlas> pixels;
    std::unordered_map<char32_t, Glyph> glyphs;
    std::map<std::string, QRectF, std::less<>> icons; // normalized atlas rects
    float ascent = 0.0f;
    float descent = 0.0f;
};

namespace
{
using Atlas = OverlayAtlasData;

struct IconSet
{
    Registry<overlayIcons::Source> registry;
    uint64_t generation = 1;
    uint64_t built = 0; // generation the current atlas was built from
};

IconSet& iconSet()
{
    static IconSet* set = [] {
        auto* s = new IconSet();
        for (const char* name : kBuiltInIcons)
            s->registry.add(name, {.tabler = QString::fromLatin1(name)});
        s->registry.addListener([s](RegistryChange, const std::string&, const std::string&) { ++s->generation; });
        return s;
    }();
    return *set;
}

/** An icon's pixels at side x side, or a null image when its source can't be read. */
QImage renderIcon(const overlayIcons::Source& src, QSize size)
{
    if (!src.tabler.isEmpty())
        return icon(src.tabler).pixmap(size, 1.0).toImage();
    if (!src.file.isEmpty())
    {
        if (src.file.endsWith(".svg", Qt::CaseInsensitive))
            return QIcon(src.file).pixmap(size, 1.0).toImage();
        QImage img(src.file);
        return img.isNull() ? img : img.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return src.image.isNull() ? QImage() : src.image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

Atlas buildAtlas()
{
    double scale = 2.0;
    if (QScreen* s = QGuiApplication::primaryScreen())
        scale = std::max(1.0, s->devicePixelRatio()) * 1.5; // a little supersampling keeps small text crisp

    QFont font("Inter");
    font.setPixelSize(int(std::round(12.0 * scale)));
    font.setWeight(QFont::Medium);
    font.setHintingPreference(QFont::PreferNoHinting);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    // Equal-width digits, so readouts don't shift as their numbers change.
    font.setFeature(QFont::Tag("tnum"), 1);
#endif
    QFontMetricsF fm(font);

    std::u32string chars;
    for (char32_t c = 32; c < 127; ++c)
        chars += c;
    chars += U"°±×–∞…²Ø";

    const int pad = 2;
    const int width = 512;
    int x = pad, y = pad, rowH = int(std::ceil(fm.height())) + pad;
    std::vector<std::pair<char32_t, QRect>> placed;
    for (char32_t c : chars)
    {
        QString s = QString::fromUcs4(&c, 1);
        QRectF b = fm.tightBoundingRect(s);
        int w = int(std::ceil(b.width())) + 2 * pad;
        if (x + w > width)
        {
            x = pad;
            y += rowH;
        }
        placed.push_back({c, QRect(x, y, w, rowH)});
        x += w + pad;
    }
    const int iconSide = int(std::ceil(kIconPx * scale));
    std::vector<std::pair<std::string, QRect>> iconCells;
    x = pad;
    y += rowH;
    for (const std::string& name : iconSet().registry.ids())
    {
        if (x + iconSide > width)
        {
            x = pad;
            y += iconSide + pad;
        }
        iconCells.push_back({name, QRect(x, y, iconSide, iconSide)});
        x += iconSide + pad;
    }
    int height = 1;
    while (height < y + std::max(rowH, iconSide) + pad)
        height *= 2;

    QImage img(width, height, QImage::Format_Grayscale8);
    img.fill(0);
    QPainter p(&img);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(font);
    p.setPen(Qt::white);

    Atlas atlas;
    atlas.ascent = float(fm.ascent() / scale);
    atlas.descent = float(fm.descent() / scale);
    for (auto& [c, cell] : placed)
    {
        QString s = QString::fromUcs4(&c, 1);
        QRectF b = fm.tightBoundingRect(s);
        // Pen position chosen so the tight box lands at the cell's top left plus padding.
        QPointF pen(cell.x() + pad - b.left(), cell.y() + pad - b.top());
        p.drawText(pen, s);

        Glyph g;
        QRectF px(cell.x(), cell.y(), b.width() + 2 * pad, b.height() + 2 * pad);
        g.uv = QRectF(px.x() / width, px.y() / height, px.width() / width, px.height() / height);
        g.box = QRectF((b.left() - pad) / scale, (b.top() - pad) / scale, px.width() / scale, px.height() / scale);
        g.advance = float(fm.horizontalAdvance(s) / scale);
        atlas.glyphs[c] = g;
    }
    // The atlas is coverage only, so an icon goes in as its alpha.
    for (const auto& [name, cell] : iconCells)
    {
        QImage src = renderIcon(*iconSet().registry.find(name), cell.size());
        if (src.isNull())
        {
            log::warn("overlay icon {}: could not be read", name);
            continue;
        }
        src = src.convertToFormat(QImage::Format_ARGB32);
        QImage cover(src.size(), QImage::Format_Grayscale8);
        for (int row = 0; row < src.height(); ++row)
        {
            const QRgb* in = reinterpret_cast<const QRgb*>(src.constScanLine(row));
            uchar* out = cover.scanLine(row);
            for (int col = 0; col < src.width(); ++col)
                out[col] = uchar(qAlpha(in[col]));
        }
        p.drawImage(cell.topLeft(), cover);
        atlas.icons[name] = QRectF(double(cell.x()) / width, double(cell.y()) / height, double(cell.width()) / width, double(cell.height()) / height);
    }
    p.end();

    // Tabular digits: every digit takes the widest one's advance, so right-aligned numbers never shift as they change.
    float digit = 0.0f;
    for (char32_t c = U'0'; c <= U'9'; ++c)
        digit = std::max(digit, atlas.glyphs[c].advance);
    for (char32_t c = U'0'; c <= U'9'; ++c)
    {
        Glyph& g = atlas.glyphs[c];
        g.box.translate((digit - g.advance) * 0.5, 0.0);
        g.advance = digit;
    }

    atlas.pixels =std::make_shared<render::OverlayAtlas>();
    atlas.pixels->width = width;
    atlas.pixels->height = height;
    atlas.pixels->pixels.resize(size_t(width) * height);
    for (int row = 0; row < height; ++row)
        std::memcpy(atlas.pixels->pixels.data() + size_t(row) * width, img.constScanLine(row), size_t(width));
    return atlas;
}

std::shared_ptr<const Atlas> currentAtlas()
{
    static std::shared_ptr<const Atlas> a;
    IconSet& set = iconSet();
    if (!a || set.built != set.generation)
    {
        set.built = set.generation;
        a = std::make_shared<const Atlas>(buildAtlas());
    }
    return a;
}

QPointF perp(QPointF d)
{
    double len = std::hypot(d.x(), d.y());
    return len > 1e-9 ? QPointF(-d.y() / len, d.x() / len) : QPointF(0, 0);
}
} // namespace

std::shared_ptr<const render::OverlayAtlas> overlayAtlas()
{
    return currentAtlas()->pixels;
}

OverlayPainter::OverlayPainter(std::vector<render::OverlayVertex>& out, double dpr) : mOut(out), mDpr(dpr) {}

OverlayPainter::~OverlayPainter() = default;

const OverlayAtlasData& OverlayPainter::atlasData() const
{
    if (!mAtlas)
        mAtlas = currentAtlas();
    return *mAtlas;
}

std::shared_ptr<const render::OverlayAtlas> OverlayPainter::atlas() const
{
    return atlasData().pixels;
}

void OverlayPainter::vertex(QPointF p, glm::vec2 uv, const glm::vec4& color)
{
    // Callers give sRGB like everything else in the UI; blending happens in linear.
    glm::vec4 c = color;
    for (int i = 0; i < 3; ++i)
        c[i] = c[i] <= 0.04045f ? c[i] / 12.92f : std::pow((c[i] + 0.055f) / 1.055f, 2.4f);
    mOut.push_back({glm::vec2(float(p.x() * mDpr), float(p.y() * mDpr)), uv, c});
}

void OverlayPainter::triangle(QPointF a, QPointF b, QPointF c, const glm::vec4& color)
{
    if (mShadow && color.a > 0.5f)
    {
        QPointF m = (a + b + c) / 3.0;
        auto grow = [&](QPointF p) {
            QPointF d = p - m;
            double len = std::hypot(d.x(), d.y());
            return len > 1e-6 ? p + d / len * 1.5 : p;
        };
        const glm::vec4 under = shadowFor(color, 0.55f);
        vertex(grow(a), {-1, 0}, under);
        vertex(grow(b), {-1, 0}, under);
        vertex(grow(c), {-1, 0}, under);
    }
    vertex(a, {-1, 0}, color);
    vertex(b, {-1, 0}, color);
    vertex(c, {-1, 0}, color);
}

void OverlayPainter::line(QPointF a, QPointF b, float width, const glm::vec4& color)
{
    if (mShadow)
    {
        mShadow = false;
        QPointF ext = b - a;
        double len = std::hypot(ext.x(), ext.y());
        QPointF pad = len > 1e-6 ? ext / len : QPointF(0, 0);
        line(a - pad, b + pad, width + 2.5f, shadowFor(color, 0.55f * color.a));
        mShadow = true;
    }
    QPointF n = perp(b - a) * (width * 0.5);
    triangle(a + n, b + n, b - n, color);
    triangle(a + n, b - n, a - n, color);
}

void OverlayPainter::dashedLine(QPointF a, QPointF b, float width, float dash, const glm::vec4& color)
{
    QPointF d = b - a;
    double len = std::hypot(d.x(), d.y());
    if (len < 1e-6)
        return;
    QPointF u = d / len;
    for (double t = 0.0; t < len; t += dash * 2.0)
        line(a + u * t, a + u * std::min(len, t + dash), width, color);
}

void OverlayPainter::polyline(const std::vector<QPointF>& pts, float width, const glm::vec4& color, bool closed)
{
    size_t n = pts.size();
    if (n < 2)
        return;
    size_t segs = closed ? n : n - 1;
    if (mShadow)
    {
        // Whole shadow first, so a later segment's shadow never darkens an earlier segment's stroke.
        mShadow = false;
        for (size_t i = 0; i < segs; ++i)
            line(pts[i], pts[(i + 1) % n], width + 2.5f, shadowFor(color, 0.55f * color.a));
        polyline(pts, width, color, closed);
        mShadow = true;
        return;
    }
    for (size_t i = 0; i < segs; ++i)
        line(pts[i], pts[(i + 1) % n], width, color);
    // Round-ish joints so thick polylines don't show notches.
    if (width > 2.0f)
        for (size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); ++i)
            disc(pts[i], width * 0.5f, color, 8);
}

void OverlayPainter::dashedPolyline(const std::vector<QPointF>& pts, float width, float dash, const glm::vec4& color, bool closed)
{
    size_t n = pts.size();
    if (n < 2 || dash <= 0.0f)
        return;
    size_t segs = closed ? n : n - 1;
    std::vector<QPointF> run;
    bool on = true;
    double left = dash; // distance remaining in the current dash or gap
    run.push_back(pts[0]);
    for (size_t i = 0; i < segs; ++i)
    {
        QPointF a = pts[i], b = pts[(i + 1) % n];
        QPointF d = b - a;
        double len = std::hypot(d.x(), d.y());
        double t = 0.0;
        while (len - t > left)
        {
            t += left;
            QPointF q = a + d * (t / len);
            if (on)
            {
                run.push_back(q);
                polyline(run, width, color);
                run.clear();
            }
            else
                run.push_back(q);
            on = !on;
            left = dash;
        }
        left -= len - t;
        if (on)
            run.push_back(b);
    }
    if (on && run.size() > 1)
        polyline(run, width, color);
}

void OverlayPainter::fill(const std::vector<QPointF>& pts, const glm::vec4& color)
{
    for (size_t i = 1; i + 1 < pts.size(); ++i)
        triangle(pts[0], pts[i], pts[i + 1], color);
}

void OverlayPainter::rect(QRectF r, const glm::vec4& color)
{
    fill({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}, color);
}

void OverlayPainter::disc(QPointF center, float radius, const glm::vec4& color, int segments)
{
    if (mShadow)
    {
        mShadow = false;
        disc(center, radius + 1.25f, shadowFor(color, 0.55f * color.a), segments);
        disc(center, radius, color, segments);
        mShadow = true;
        return;
    }
    for (int i = 0; i < segments; ++i)
    {
        double a0 = 2.0 * std::numbers::pi * i / segments, a1 = 2.0 * std::numbers::pi * (i + 1) / segments;
        triangle(center, center + QPointF(std::cos(a0), std::sin(a0)) * radius, center + QPointF(std::cos(a1), std::sin(a1)) * radius,
                 color);
    }
}

float OverlayPainter::textWidth(const QString& s)
{
    const std::shared_ptr<const Atlas> keep = currentAtlas();
    const Atlas& a = *keep;
    float w = 0.0f;
    for (char32_t c : s.toUcs4())
        if (auto it = a.glyphs.find(c); it != a.glyphs.end())
            w += it->second.advance;
    return w;
}

float OverlayPainter::text(QPointF at, const QString& s, const glm::vec4& color, Align align)
{
    if (mShadow)
    {
        mShadow = false;
        text(at + QPointF(1, 1), s, shadowFor(color, 0.7f * color.a), align);
        mShadow = true;
    }
    const Atlas& a = atlasData();
    float w = textWidth(s);
    double x = at.x() - (align == Center ? w * 0.5 : align == Right ? w : 0.0);
    // Snap to whole device pixels so glyphs don't smear.
    x = std::round(x * mDpr) / mDpr;
    double y = std::round(at.y() * mDpr) / mDpr;
    for (char32_t c : s.toUcs4())
    {
        auto it = a.glyphs.find(c);
        if (it == a.glyphs.end())
            continue;
        const Glyph& g = it->second;
        QRectF r = g.box.translated(x, y);
        glm::vec2 t0(float(g.uv.left()), float(g.uv.top())), t1(float(g.uv.right()), float(g.uv.bottom()));
        vertex(r.topLeft(), t0, color);
        vertex(r.topRight(), {t1.x, t0.y}, color);
        vertex(r.bottomRight(), t1, color);
        vertex(r.topLeft(), t0, color);
        vertex(r.bottomRight(), t1, color);
        vertex(r.bottomLeft(), {t0.x, t1.y}, color);
        x += g.advance;
    }
    return w;
}

bool OverlayPainter::icon(QRectF r, std::string_view name, const glm::vec4& color)
{
    const Atlas& a = atlasData();
    auto it = a.icons.find(name);
    if (it == a.icons.end())
        return false;
    if (mShadow)
    {
        // An outline from the icon's own shape: its coverage stamped around it in the contrasting shade.
        mShadow = false;
        const glm::vec4 under = shadowFor(color, 0.6f * color.a);
        for (QPointF d : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1), QPointF(1, 1)})
            icon(r.translated(d), name, under);
        mShadow = true;
    }
    QRectF uv = it->second;
    glm::vec2 t0(float(uv.left()), float(uv.top())), t1(float(uv.right()), float(uv.bottom()));
    vertex(r.topLeft(), t0, color);
    vertex(r.topRight(), {t1.x, t0.y}, color);
    vertex(r.bottomRight(), t1, color);
    vertex(r.topLeft(), t0, color);
    vertex(r.bottomRight(), t1, color);
    vertex(r.bottomLeft(), {t0.x, t1.y}, color);
    return true;
}

glm::vec4 OverlayPainter::shadowFor(const glm::vec4& c, float a)
{
    // Only dark grays (ink() on a bright backdrop) get a light halo; tinted handles keep their dark one.
    bool gray = std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}) < 0.02f;
    return gray && c.g < 0.5f ? glm::vec4(1.0f, 1.0f, 1.0f, a * 0.8f) : glm::vec4(0.0f, 0.0f, 0.0f, a);
}

void OverlayPainter::setBackdrop(std::vector<float> bright, int cols, int rows, QSizeF view)
{
    mBackdrop = std::move(bright);
    mBackdropCols = cols;
    mBackdropRows = rows;
    mBackdropView = view;
    mBackdropMean = 0.0f;
    if (mBackdrop.size() != size_t(cols) * size_t(rows) || mBackdrop.empty())
    {
        mBackdrop.clear();
        return;
    }
    for (float b : mBackdrop)
        mBackdropMean += b;
    mBackdropMean /= float(mBackdrop.size());
}

void OverlayPainter::classifyBackdrop(const std::vector<float>& luminance, std::vector<float>& state)
{
    // Black and white text contrast equally at a relative luminance of about 0.18; switch a little either side of it.
    constexpr float kToBright = 0.21f, kToDark = 0.15f;
    if (state.size() != luminance.size())
    {
        state.assign(luminance.size(), 0.0f);
        for (size_t i = 0; i < luminance.size(); ++i)
            state[i] = luminance[i] > 0.18f ? 1.0f : 0.0f;
        return;
    }
    for (size_t i = 0; i < luminance.size(); ++i)
        if (state[i] > 0.5f ? luminance[i] < kToDark : luminance[i] > kToBright)
            state[i] = 1.0f - state[i];
}

bool OverlayPainter::brightBackdrop(QRectF r) const
{
    if (mBackdrop.empty())
        return false;
    if (r.isEmpty() || mBackdropView.isEmpty())
        return mBackdropMean > 0.5f;
    double cw = mBackdropView.width() / mBackdropCols, ch = mBackdropView.height() / mBackdropRows;
    int c0 = std::clamp(int(std::floor(r.left() / cw)), 0, mBackdropCols - 1), c1 = std::clamp(int(std::floor(r.right() / cw)), 0, mBackdropCols - 1);
    int r0 = std::clamp(int(std::floor(r.top() / ch)), 0, mBackdropRows - 1), r1 = std::clamp(int(std::floor(r.bottom() / ch)), 0, mBackdropRows - 1);
    float sum = 0.0f;
    for (int y = r0; y <= r1; ++y)
        for (int x = c0; x <= c1; ++x)
            sum += mBackdrop[size_t(y) * size_t(mBackdropCols) + size_t(x)];
    return sum > 0.5f * float((r1 - r0 + 1) * (c1 - c0 + 1));
}

glm::vec4 OverlayPainter::ink(float shade, float alpha, QRectF r) const
{
    // Dark ink stays well below a mid-gray backdrop even for dim shades, so they never vanish into it.
    if (brightBackdrop(r))
    {
        float g = 0.35f * (1.0f - shade);
        return {g, g, g, alpha};
    }
    glm::vec4 tint = themeSrgb("hudText", glm::vec4(1.0f));
    return {shade * tint.r, shade * tint.g, shade * tint.b, alpha};
}

void OverlayPainter::label(QPointF at, const QString& s, const glm::vec4& color)
{
    const Atlas& a = atlasData();
    float w = textWidth(s);
    QRectF plate(at.x() - 5, at.y() - a.ascent - 3, w + 10, a.ascent + a.descent + 6);
    rect(plate, {0.08f, 0.08f, 0.08f, 0.78f});
    text(at, s, color);
}

namespace overlayIcons
{
void add(std::string id, Source source, std::string owner)
{
    iconSet().registry.put(std::move(id), std::move(source), std::move(owner));
}

ext::Context::Handle add(ext::Context& ctx, std::string id, Source source)
{
    try
    {
        return ctx.add(iconSet().registry, "overlayIcon", std::move(id), std::move(source));
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: overlay icon: {}", ctx.owner(), e.what());
        return 0;
    }
}

bool remove(std::string_view id) { return iconSet().registry.remove(id); }
size_t removeOwned(std::string_view owner) { return iconSet().registry.removeOwned(owner); }
bool contains(std::string_view id) { return iconSet().registry.contains(id); }
std::vector<std::string> ids() { return iconSet().registry.ids(); }
uint64_t generation() { return iconSet().generation; }
} // namespace overlayIcons
} // namespace lila::ui
