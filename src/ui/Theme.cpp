#include "ui/Theme.h"

#include "core/Paths.h"
#include "core/PrefStore.h"
#include "render/Renderer.h"
#include "ui/DocsUi.h"
#include "ui/Telemetry.h"
#include "ui/ThemeModel.h"
#include "ui/ViewportDisplay.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QProxyStyle>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace lila::ui
{
namespace
{
/**
 * Lila's style over Fusion. Alt belongs to the hotkeys (Alt+letter bindings, Alt-drag camera), so a lone Alt never
 * focuses the menu bar. Selected and hovered rows in lists and trees get a rounded, inset highlight: rounded at the
 * row's two ends, square between its cells.
 */
class NoAltMenuStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const override
    {
        if (const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option))
        {
            // The row's own fill keeps alternating colours only; the cells draw the selection.
            if (element == PE_PanelItemViewRow && (item->state & State_Selected))
            {
                if (item->features & QStyleOptionViewItem::Alternate)
                    painter->fillRect(item->rect, item->palette.brush(QPalette::AlternateBase));
                return;
            }
            if (element == PE_PanelItemViewItem && drawRowHighlight(*item, painter))
                return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* ret) const override
    {
        if (hint == SH_MenuBar_AltKeyNavigation)
            return 0;
        return QProxyStyle::styleHint(hint, option, widget, ret);
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size, const QWidget* widget) const override
    {
        if (const auto* item = section(type == CT_MenuItem, option))
            return {size.width(), item->fontMetrics.height() + 6};
        return QProxyStyle::sizeFromContents(type, option, size, widget);
    }

    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const override
    {
        if (element == CE_MenuTearoff)
            return drawTearRow(painter, option->rect, option->palette, option->state & State_Selected, {});
        const auto* item = section(element == CE_MenuItem, option);
        if (!item)
            return QProxyStyle::drawControl(element, option, painter, widget);
        painter->save();
        painter->setFont(item->font);
        // In a menu that tears off, a section title is its own tear-off row (see TearOff.cpp).
        if (widget && widget->property("lilaTearOff").toBool())
            drawTearRow(painter, item->rect, item->palette, widget->property("lilaHotSection").toRect() == item->rect, item->text);
        else
        {
            // Maya's titled separator: the title dimmed, then a rule running on from it to the menu's edge.
            QRect r = item->rect.adjusted(8, 2, -4, 0);
            painter->setPen(item->palette.color(QPalette::Disabled, QPalette::Text));
            QRect text;
            painter->drawText(r, Qt::AlignLeft | Qt::AlignVCenter, item->text, &text);
            painter->setPen(item->palette.color(QPalette::Midlight));
            if (text.right() + 6 < r.right())
                painter->drawLine(text.right() + 6, r.center().y() + 1, r.right(), r.center().y() + 1);
        }
        painter->restore();
    }

private:
    /** A rounded highlight for a selected or hovered cell; false leaves the cell to Fusion. */
    static bool drawRowHighlight(const QStyleOptionViewItem& item, QPainter* p)
    {
        const bool selected = item.state & State_Selected;
        const bool hover = (item.state & State_MouseOver) && (item.state & State_Enabled);
        if (!selected && !hover)
            return false;
        if (item.backgroundBrush.style() != Qt::NoBrush)
            p->fillRect(item.rect, item.backgroundBrush);
        QColor fill;
        if (selected)
            fill = item.palette.color(item.state & State_Active ? QPalette::Active : QPalette::Inactive, QPalette::Highlight);
        else
        {
            fill = themeColor("hover");
            fill.setAlphaF(0.25f);
        }
        using Pos = QStyleOptionViewItem::ViewItemPosition;
        const Pos pos = item.viewItemPosition;
        const bool first = pos == Pos::Beginning || pos == Pos::OnlyOne || pos == Pos::Invalid;
        const bool last = pos == Pos::End || pos == Pos::OnlyOne || pos == Pos::Invalid;
        constexpr qreal kRadius = 3, kInset = 2;
        // Inset at the ends only, and widened under the corners between cells, so a row's cells join into one bar.
        QRectF r = QRectF(item.rect).adjusted(first ? kInset : -kRadius, 1, last ? -kInset : kRadius, -1);
        p->save();
        p->setClipRect(item.rect);
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(fill);
        p->drawRoundedRect(r, kRadius, kRadius);
        p->restore();
        return true;
    }

    /** The menu's dashed tear-off row, with a section's title at its left when it has one. */
    static void drawTearRow(QPainter* p, const QRect& rect, const QPalette& pal, bool hot, const QString& title)
    {
        p->fillRect(rect, pal.brush(hot ? QPalette::Highlight : QPalette::Button));
        int left = rect.x() + 2;
        if (!title.isEmpty())
        {
            QRect text;
            p->setPen(hot ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Disabled, QPalette::Text));
            p->drawText(rect.adjusted(8, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter, title, &text);
            left = text.right() + 6;
        }
        int y = rect.y() + rect.height() / 2;
        int right = rect.x() + rect.width() - 4;
        if (left >= right)
            return;
        p->setPen(QPen(pal.dark().color(), 1, Qt::DashLine));
        p->drawLine(left, y - 1, right, y - 1);
        p->setPen(QPen(pal.light().color(), 1, Qt::DashLine));
        p->drawLine(left, y, right, y);
    }

    static const QStyleOptionMenuItem* section(bool menuItem, const QStyleOption* option)
    {
        const auto* item = menuItem ? qstyleoption_cast<const QStyleOptionMenuItem*>(option) : nullptr;
        return item && item->menuItemType == QStyleOptionMenuItem::Separator && !item->text.isEmpty() ? item : nullptr;
    }
};

bool monotone() { return PrefStore::get().value<bool>("general", ui::viewportDisplay::kMonotone, false); }

/** The colour icon as luminance, alpha kept. */
QIcon grayscaleIcon(const QString& path)
{
    QImage img = QImage(path).convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y)
    {
        auto* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x)
        {
            int g = qGray(row[x]);
            row[x] = qRgba(g, g, g, qAlpha(row[x]));
        }
    }
    return QIcon(QPixmap::fromImage(img));
}

QIcon appIcon(bool mono)
{
    const QString color = QString::fromStdWString((paths::resourcesDir() / "icons" / "lila_color.png").wstring());
    return mono ? grayscaleIcon(color) : QIcon(color);
}

/**
 * Gives every top-level window (dialogs, floating docks, tool windows) the dark native title bar, and the grey icon in
 * it. The taskbar keeps the colour icon (the window's big icon) unless the UI is monotone.
 */
class DarkTitleBars : public QObject
{
public:
    explicit DarkTitleBars(QObject* parent) : QObject(parent), mMono(monotone()) {}

    /** Follows the monotone pref: the app icon and every window's big icon go grey or back to colour. */
    void setMonotone(bool mono)
    {
        if (mono == mMono)
            return;
        mMono = mono;
        qApp->setWindowIcon(appIcon(mono));
#ifdef _WIN32
        for (auto& [dpi, icon] : mBigIcons)
            DestroyIcon(icon);
        mBigIcons.clear();
        for (QWidget* w : QApplication::topLevelWidgets())
            if (w->testAttribute(Qt::WA_WState_Created))
                applyIcons(HWND(w->winId()));
#endif
    }

#ifdef _WIN32
    ~DarkTitleBars() override
    {
        if (mSmallIcon)
            DestroyIcon(mSmallIcon);
        for (auto& [dpi, icon] : mBigIcons)
            DestroyIcon(icon);
    }
#endif

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
#ifdef _WIN32
        // Qt sends its own icons again when the window icon changes, so ours go back on after.
        if (e->type() == QEvent::Show || e->type() == QEvent::WinIdChange || e->type() == QEvent::WindowIconChange)
            if (auto* w = qobject_cast<QWidget*>(obj); w && w->isWindow() && w->testAttribute(Qt::WA_WState_Created))
            {
                BOOL dark = themes::current()->base != "light";
                HWND hwnd = HWND(w->winId());
                DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
                applyIcons(hwnd);
            }
#else
        (void)e;
        (void)obj;
#endif
        return false;
    }

private:
    bool mMono = false;

#ifdef _WIN32
    void applyIcons(HWND hwnd)
    {
        if (HICON icon = smallIcon())
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, LPARAM(icon));
        if (HICON icon = bigIcon(GetDpiForWindow(hwnd)))
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, LPARAM(icon));
    }

    HICON smallIcon()
    {
        if (!mSmallIcon)
        {
            auto path = paths::resourcesDir() / "icons" / "lila_grey.ico";
            int size = GetSystemMetrics(SM_CXSMICON);
            mSmallIcon = HICON(LoadImageW(nullptr, path.wstring().c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
        }
        return mSmallIcon;
    }

    // Qt builds the big icon at the 96-dpi size, which the taskbar shows at half size on a scaled display.
    HICON bigIcon(UINT dpi)
    {
        HICON& icon = mBigIcons[dpi];
        if (!icon)
        {
            auto path = paths::resourcesDir() / "icons" / (mMono ? "lila_grey.ico" : "lila.ico");
            int size = GetSystemMetricsForDpi(SM_CXICON, dpi);
            icon = HICON(LoadImageW(nullptr, path.wstring().c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
        }
        return icon;
    }

    HICON mSmallIcon = nullptr;
    std::map<UINT, HICON> mBigIcons;
#endif
};

void loadBundledFonts()
{
    auto dir = paths::resourcesDir() / "fonts";
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.path().extension() == ".ttf")
            QFontDatabase::addApplicationFont(QString::fromStdWString(entry.path().wstring()));
}
} // namespace

QFont monoFont()
{
    PrefStore& p = PrefStore::get();
    QFont f(QString::fromStdString(p.value<std::string>("general", "/ui/monoFont", "JetBrains Mono")));
    f.setStyleHint(QFont::Monospace);
    f.setPointSizeF(p.value<double>("general", "/ui/monoFontSize", 9.5));
    return f;
}

namespace
{
QColor qcolor(themes::Rgba c) { return QColor(c.r, c.g, c.b, c.a); }

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

/** Whether a theme's neutral ramp is Monotone's, so greys map to themselves and nothing needs rewriting. */
bool monotoneRamp(const themes::Theme& t)
{
    for (const themes::TokenInfo& info : themes::tokens())
        if (info.level >= 0 && !themes::isDefault(t, info.id))
            return false;
    return true;
}

bool gFirstRun = false;
/** The resolved theme last pushed into the palette, to skip prefs changes that leave it alone. */
std::string gApplied;

QString appStyleSheet(const themes::Theme& t)
{
    auto c = [&](const char* id) { return QString::fromStdString(themes::formatColor(t.color(id))); };
    return QString(R"(
        QToolTip { border: 1px solid %1; padding: 3px; }
        QDockWidget::title { background: %2; padding: 4px 6px; }
        QMainWindow::separator { background: %3; width: 3px; height: 3px; }
        QMainWindow::separator:hover { background: %4; }
        QStatusBar { background: %5; }
        QStatusBar QToolButton { border: none; padding: 1px 8px; }
        QStatusBar QToolButton:checked { background: %6; }
        QTabBar#shelfTabs::tab { background: transparent; border: none; border-bottom: 2px solid transparent;
                                 padding: 4px 10px 3px; margin-right: 2px; color: %7; }
        QTabBar#shelfTabs::tab:hover { color: %8; }
        QTabBar#shelfTabs::tab:selected { color: %8; border-bottom-color: %9; }
        QWidget#shelfPanel QToolButton { border: none; border-radius: 4px; padding: 2px 6px; background: transparent; }
        QWidget#shelfPanel QToolButton:hover { background: %10; }
        QWidget#shelfPanel QToolButton:pressed { background: %6; }
    )")
        .arg(c("border"), c("panel"), c("separator"), c("hover"), c("statusBar"), c("checked"), c("dimText"), c("text"), c("accent"))
        .arg(c("button"));
}

void applyColors(QApplication& app)
{
    std::shared_ptr<const themes::Theme> theme = themes::current();
    const themes::Theme& t = *theme;
    auto tok = [&](const char* id) { return qcolor(t.color(id)); };
    auto gray = [&](int v) { return qcolor(themes::shade(t, v)); };
    QPalette p;
    p.setColor(QPalette::Window, tok("window"));
    p.setColor(QPalette::WindowText, tok("text"));
    p.setColor(QPalette::Base, tok("base"));
    p.setColor(QPalette::AlternateBase, tok("altBase"));
    p.setColor(QPalette::ToolTipBase, tok("tooltip"));
    p.setColor(QPalette::ToolTipText, tok("tooltipText"));
    p.setColor(QPalette::PlaceholderText, tok("disabled"));
    p.setColor(QPalette::Text, tok("text"));
    p.setColor(QPalette::Button, tok("button"));
    p.setColor(QPalette::ButtonText, tok("text"));
    p.setColor(QPalette::BrightText, tok("brightText"));
    p.setColor(QPalette::Highlight, tok("highlight"));
    p.setColor(QPalette::HighlightedText, tok("highlightText"));
    p.setColor(QPalette::Link, tok("accent"));
    p.setColor(QPalette::LinkVisited, gray(190));
    p.setColor(QPalette::Light, gray(78));
    p.setColor(QPalette::Midlight, gray(66));
    p.setColor(QPalette::Mid, gray(40));
    p.setColor(QPalette::Dark, gray(30));
    p.setColor(QPalette::Shadow, tok("shadow"));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, tok("disabled"));
    p.setColor(QPalette::Disabled, QPalette::Highlight, gray(70));
    app.setPalette(p);

    QPalette menu = p;
    menu.setColor(QPalette::Base, tok("menu"));
    app.setPalette(menu, "QMenu");
    QPalette input = p;
    input.setColor(QPalette::Base, tok("input"));
    app.setPalette(input, "QLineEdit");
    app.setPalette(input, "QAbstractSpinBox");
    QPalette scroll = p;
    scroll.setColor(QPalette::Button, tok("scrollbar"));
    app.setPalette(scroll, "QScrollBar");

    QString sheet = appStyleSheet(t);
    if (app.styleSheet() != sheet)
        app.setStyleSheet(sheet);
    gApplied = themes::toText(t, true);
}

/**
 * Keeps widget style sheets on the theme: the sheet a widget is given is kept as its template, and what it shows is
 * that template with the greys mapped. Re-run over every widget when the theme changes.
 */
class StyleSheetThemer : public QObject
{
public:
    using QObject::QObject;

    static void retheme(QWidget* w)
    {
        if (w->property("lilaRawColors").toBool())
            return;
        QVariant tmpl = w->property("lilaQssTemplate");
        if (!tmpl.isValid())
            return;
        QString themed = themedStyleSheet(tmpl.toString());
        if (themed != w->styleSheet())
        {
            w->setProperty("lilaQssApplied", themed);
            w->setStyleSheet(themed);
        }
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::StyleChange && e->type() != QEvent::Polish)
            return false;
        auto* w = qobject_cast<QWidget*>(obj);
        if (!w || w->property("lilaRawColors").toBool())
            return false;
        QString sheet = w->styleSheet();
        if (sheet.isEmpty() || sheet == w->property("lilaQssApplied").toString())
            return false;
        // A sheet set by the widget's own code: remember it, then show it themed.
        w->setProperty("lilaQssTemplate", sheet);
        w->setProperty("lilaQssApplied", sheet);
        retheme(w);
        return false;
    }
};

void followTheme(QApplication& app)
{
    applyColors(app);
    for (QWidget* w : QApplication::allWidgets())
    {
        StyleSheetThemer::retheme(w);
        w->update();
#ifdef _WIN32
        if (w->isWindow() && w->testAttribute(Qt::WA_WState_Created))
        {
            BOOL dark = themes::current()->base != "light";
            DwmSetWindowAttribute(HWND(w->winId()), 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        }
#endif
    }
}

class ThemeCard : public QFrame
{
public:
    ThemeCard(const themes::Theme& t, QWidget* parent) : QFrame(parent), mTheme(t)
    {
        setFixedSize(206, 178);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QString::fromStdString(t.description));
    }

    std::function<void()> picked;
    bool selected = false;

protected:
    void mousePressEvent(QMouseEvent*) override
    {
        if (picked)
            picked();
    }

    void paintEvent(QPaintEvent*) override
    {
        // Drawn in the card's own theme, whatever the current one is.
        auto c = [&](const char* id) { return qcolor(mTheme.color(id)); };
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        p.setPen(QPen(selected ? c("highlight") : c("border"), selected ? 3.0 : 1.0));
        p.setBrush(c("window"));
        p.drawRoundedRect(r, 6, 6);
        QRectF title(r.left() + 8, r.top() + 8, r.width() - 16, 14);
        p.setPen(Qt::NoPen);
        p.setBrush(c("panel"));
        p.drawRoundedRect(title, 2, 2);
        QRectF list(r.left() + 8, title.bottom() + 4, 64, 54);
        p.setBrush(c("base"));
        p.drawRect(list);
        p.setBrush(c("highlight"));
        p.drawRect(QRectF(list.left(), list.top() + 18, list.width(), 12));
        for (int i = 0; i < 4; ++i)
        {
            p.setBrush(i == 1 ? c("highlightText") : c("text"));
            p.drawRect(QRectF(list.left() + 5, list.top() + 6 + i * 12 + 1, 30 + (i * 7) % 20, 2));
        }
        QRectF view(list.right() + 6, list.top(), r.right() - 8 - list.right() - 6, list.height());
        p.setBrush(c("viewportBackground"));
        p.drawRect(view);
        QPointF o = view.center() + QPointF(-8, 10);
        p.setPen(QPen(c("axisX"), 2));
        p.drawLine(o, o + QPointF(24, 0));
        p.setPen(QPen(c("axisY"), 2));
        p.drawLine(o, o + QPointF(0, -24));
        p.setPen(QPen(c("axisZ"), 2));
        p.drawLine(o, o + QPointF(-12, 10));
        p.setPen(QPen(c("wireSelected"), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(view.right() - 26, view.top() + 8, 16, 16));

        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.setPen(c("text"));
        QRectF text(r.left() + 8, list.bottom() + 6, r.width() - 16, r.bottom() - list.bottom() - 10);
        p.drawText(text, Qt::AlignLeft | Qt::AlignTop, QString::fromStdString(mTheme.name));
        f.setBold(false);
        f.setPointSizeF(f.pointSizeF() * 0.92);
        p.setFont(f);
        p.setPen(c("dimText"));
        p.drawText(text.adjusted(0, QFontMetrics(font()).height() + 2, 0, 0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   QString::fromStdString(mTheme.description));
    }

private:
    themes::Theme mTheme;
};
} // namespace

QLabel* emptyStateLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName("emptyState");
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setContentsMargins(16, 24, 16, 24);
    QPalette pal = label->palette();
    pal.setColor(QPalette::WindowText, themeColor("dimText"));
    label->setPalette(pal);
    return label;
}

QColor themeColor(std::string_view token)
{
    return qcolor(themes::current()->color(token));
}

QColor themeShade(int level, int alpha)
{
    return qcolor(themes::shade(level, uint8_t(std::clamp(alpha, 0, 255))));
}

glm::vec4 themeSrgb(std::string_view token, const glm::vec4& legacy)
{
    std::shared_ptr<const themes::Theme> t = themes::current();
    if (themes::isDefault(*t, token))
        return legacy;
    themes::Rgba c = t->color(token);
    return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, legacy.a};
}

glm::vec4 themeLinear(std::string_view token, const glm::vec4& legacyLinear)
{
    std::shared_ptr<const themes::Theme> t = themes::current();
    if (themes::isDefault(*t, token))
        return legacyLinear;
    themes::Rgba c = t->color(token);
    return {srgbToLinear(c.r / 255.0f), srgbToLinear(c.g / 255.0f), srgbToLinear(c.b / 255.0f), legacyLinear.a};
}

QString themedStyleSheet(const QString& qss)
{
    std::shared_ptr<const themes::Theme> t = themes::current();
    if (monotoneRamp(*t))
        return qss;
    static const QRegularExpression hex(R"((?<=[:\s,])#([0-9a-fA-F]{2})([0-9a-fA-F]{2})([0-9a-fA-F]{2})\b)");
    static const QRegularExpression rgb(R"(rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))");
    auto mapped = [&](int v) { return QString::fromStdString(themes::formatColor(themes::shade(*t, v))); };
    QString out;
    for (const QRegularExpression* re : {&hex, &rgb})
    {
        const QString& in = re == &hex ? qss : out;
        QString next;
        qsizetype last = 0;
        for (auto it = re->globalMatch(in); it.hasNext();)
        {
            QRegularExpressionMatch m = it.next();
            int base = re == &hex ? 16 : 10;
            int r = m.captured(1).toInt(nullptr, base), g = m.captured(2).toInt(nullptr, base), b = m.captured(3).toInt(nullptr, base);
            next += in.mid(last, m.capturedStart() - last);
            next += r == g && g == b ? mapped(r) : m.captured(0);
            last = m.capturedEnd();
        }
        next += in.mid(last);
        out = next;
    }
    return out;
}

bool themeChooserPending()
{
    return gFirstRun && !PrefStore::get().value<bool>("general", "/ui/themeChosen", false);
}

void setThemeChooserDone()
{
    PrefStore::get().set("general", "/ui/themeChosen", true);
}

QWidget* makeThemeChooser(QWidget* parent)
{
    auto* box = new QWidget(parent);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel("Choose a look", box);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.25);
    title->setFont(f);
    layout->addWidget(title);
    auto* hint = new QLabel("You can change it any time in Preferences > Interface > Theme.", box);
    hint->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(hint);
    auto* grid = new QGridLayout();
    grid->setSpacing(10);
    auto cards = std::make_shared<std::vector<ThemeCard*>>();
    for (const char* name : {"Colour", "Monotone", "Light", "Midnight", "Ember", "Paper"})
    {
        const themes::Theme* t = themes::builtIn(name);
        auto* card = new ThemeCard(*t, box);
        card->selected = themes::currentName() == name;
        card->picked = [cards, card, n = std::string(name)] {
            for (ThemeCard* c : *cards)
            {
                c->selected = c == card;
                c->update();
            }
            themes::setCurrent(n);
            setThemeChooserDone();
        };
        grid->addWidget(card, int(cards->size()) / 3, int(cards->size()) % 3);
        cards->push_back(card);
    }
    grid->setColumnStretch(3, 1);
    layout->addLayout(grid);

    namespace vd = viewportDisplay;
    layout->addSpacing(12);
    auto* qualityTitle = new QLabel("Viewport quality", box);
    qualityTitle->setFont(f);
    layout->addWidget(qualityTitle);
    auto* qualityHint = new QLabel("Only the starting point: each of these toggles from the viewport toolbar any time.", box);
    qualityHint->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(qualityHint);
    const vd::Quality recommended = vd::recommendedQuality();
    const std::string gpu = render::Renderer::get().gpuInfo().name;
    vd::applyQuality(recommended);
    struct Option
    {
        vd::Quality quality;
        const char* name;
        const char* detail;
    };
    const Option options[] = {
        {vd::Quality::Best, "Best quality", "Accurate ambient occlusion, 8x antialiasing and shadows."},
        {vd::Quality::Balanced, "Balanced", "Faster ambient occlusion that also darkens reflections, 4x antialiasing and shadows."},
        {vd::Quality::Fastest, "Fastest", "No ambient occlusion, antialiasing or shadows. For laptops and integrated graphics."},
    };
    auto* group = new QButtonGroup(box);
    for (const Option& o : options)
    {
        QString label = o.name;
        if (o.quality == recommended)
            label += gpu.empty() ? QString(" (recommended)") : QString(" (recommended for %1)").arg(QString::fromStdString(gpu));
        auto* radio = new QRadioButton(label, box);
        radio->setChecked(o.quality == recommended);
        group->addButton(radio);
        QObject::connect(radio, &QRadioButton::toggled, radio, [q = o.quality](bool on) {
            if (on)
                vd::applyQuality(q);
        });
        auto* detail = new QLabel(o.detail, box);
        detail->setForegroundRole(QPalette::PlaceholderText);
        detail->setContentsMargins(22, 0, 0, 4);
        layout->addWidget(radio);
        layout->addWidget(detail);
    }
    return box;
}

namespace
{
/** Stays until Continue: Esc and the close button do nothing. */
class SetupDialog : public QDialog
{
public:
    using QDialog::QDialog;

    void reject() override {}

protected:
    void closeEvent(QCloseEvent* e) override { e->ignore(); }
};

QWidget* makeTelemetryChoice(QWidget* parent)
{
    auto* box = new QWidget(parent);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel("Help improve Lila", box);
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.25);
    title->setFont(f);
    layout->addWidget(title);
    auto* hint = new QLabel("Lila is made by one developer. Anonymous statistics show which tools get used and where Lila crashes, so it can be fixed. "
                            "Never your files, file names or scene content. Change it any time in "
                            "Preferences > General > Telemetry.",
                            box);
    hint->setForegroundRole(QPalette::PlaceholderText);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    for (auto [label, key] : {std::pair{"Send usage statistics (version, GPU, session length, which commands are used)", kTelemetryUsagePref},
                              std::pair{"Send crash reports", kTelemetryCrashPref}})
    {
        auto* check = new QCheckBox(label, box);
        check->setChecked(PrefStore::get().value<bool>("general", key, false));
        QObject::connect(check, &QCheckBox::toggled, check, [key](bool on) { PrefStore::get().set("general", key, on); });
        layout->addWidget(check);
    }

    auto* about = new QLabel("Optional, sent with usage statistics:", box);
    about->setContentsMargins(0, 6, 0, 0);
    layout->addWidget(about);
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    for (auto [label, key, choices] : {std::tuple{"Your role", telemetry::kRolePref, &telemetry::roleChoices()},
                                       std::tuple{"Team size", telemetry::kTeamSizePref, &telemetry::teamSizeChoices()},
                                       std::tuple{"Used before Lila", telemetry::kPreviousDccPref, &telemetry::previousDccChoices()}})
    {
        auto* combo = new QComboBox(box);
        const std::string current = PrefStore::get().value<std::string>("general", key, "");
        for (const auto& [shown, value] : *choices)
        {
            combo->addItem(QString::fromStdString(shown), QString::fromStdString(value));
            if (value == current)
                combo->setCurrentIndex(combo->count() - 1);
        }
        QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [combo, key](int) {
            PrefStore::get().set("general", key, combo->currentData().toString().toStdString());
        });
        form->addRow(label, combo);
    }
    layout->addLayout(form);
    auto* sent = new QPushButton("See Exactly What's Sent...", box);
    QObject::connect(sent, &QPushButton::clicked, sent, [box] { telemetry::showPending(box->window()); });
    auto* details = new QPushButton("Privacy Details...", box);
    QObject::connect(details, &QPushButton::clicked, details, [] { docs::open("privacy", docs::Route::Browser); });
    auto* sentRow = new QHBoxLayout;
    sentRow->addStretch(1);
    sentRow->addWidget(sent);
    sentRow->addWidget(details);
    sentRow->addStretch(1);
    layout->addLayout(sentRow);
    return box;
}
} // namespace

void runFirstRunSetup(QWidget* parent)
{
    // Usage statistics start ticked here, where they are seen; everywhere else an unset pref means off.
    if (PrefStore::get().value("general", kTelemetryUsagePref).is_null())
        PrefStore::get().set("general", kTelemetryUsagePref, true);
    SetupDialog dlg(parent);
    dlg.setWindowTitle("Welcome to Lila");
    dlg.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    dlg.setWindowModality(Qt::ApplicationModal);
    auto* layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(20, 16, 20, 14);
    layout->addWidget(makeThemeChooser(&dlg));
    layout->addSpacing(12);
    layout->addWidget(makeTelemetryChoice(&dlg));
    layout->addSpacing(8);
    auto* buttons = new QDialogButtonBox(&dlg);
    QPushButton* go = buttons->addButton("Continue", QDialogButtonBox::AcceptRole);
    go->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    layout->addWidget(buttons);
    dlg.exec();
    setThemeChooserDone();
}

void showTelemetrySettings(QWidget* parent)
{
    static QPointer<QDialog> open;
    if (!open)
    {
        open = new QDialog(parent);
        open->setAttribute(Qt::WA_DeleteOnClose);
        open->setWindowTitle("Telemetry and Privacy");
        auto* layout = new QVBoxLayout(open);
        layout->setContentsMargins(20, 16, 20, 14);
        layout->addWidget(makeTelemetryChoice(open));
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, open);
        QObject::connect(buttons, &QDialogButtonBox::rejected, open, &QDialog::close);
        layout->addWidget(buttons);
    }
    open->show();
    open->raise();
    open->activateWindow();
}

void applyTheme(QApplication& app)
{
    app.setStyle(new NoAltMenuStyle(QStyleFactory::create("Fusion")));

    PrefStore& prefs = PrefStore::get();
    std::error_code ec;
    gFirstRun = !std::filesystem::exists(prefs.userDir() / "general.json", ec) && prefs.value("general", themes::kThemePref).is_null() &&
                QGuiApplication::platformName() != "offscreen";

    loadBundledFonts();
    app.setWindowIcon(appIcon(monotone()));
    QFont ui(QString::fromStdString(prefs.value<std::string>("general", "/ui/font", "Inter")));
    ui.setPointSizeF(prefs.value<double>("general", "/ui/fontSize", 9.0));
    ui.setHintingPreference(QFont::PreferNoHinting);
    // Tabular figures, so numbers in fields don't shift width as they change.
    ui.setFeature(QFont::Tag("tnum"), 1);
    app.setFont(ui);

    applyColors(app);
    app.installEventFilter(new StyleSheetThemer(&app));

    auto* titleBars = new DarkTitleBars(&app);
    app.installEventFilter(titleBars);
    QPointer<DarkTitleBars> bars(titleBars);
    PrefStore::get().onChanged([bars](const std::string& file) {
        if (file != "general")
            return;
        // Deferred: the theme cache drops on this same change, and every listener must have run first.
        QTimer::singleShot(0, qApp, [bars] {
            if (!bars)
                return;
            bars->setMonotone(monotone());
            if (themes::toText(*themes::current(), true) != gApplied)
                followTheme(*qApp);
        });
    });
}
} // namespace lila::ui
