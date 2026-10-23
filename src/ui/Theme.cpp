#include "ui/Theme.h"

#include "core/Paths.h"
#include "core/PrefStore.h"
#include "ui/ViewportDisplay.h"

#include <QApplication>
#include <QFontDatabase>
#include <QIcon>
#include <QImage>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QWidget>

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
/** Alt belongs to the hotkeys (Alt+letter bindings, Alt-drag camera), so a lone Alt never focuses the menu bar. */
class NoAltMenuStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* ret) const override
    {
        if (hint == SH_MenuBar_AltKeyNavigation)
            return 0;
        return QProxyStyle::styleHint(hint, option, widget, ret);
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
                BOOL dark = TRUE;
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

void applyTheme(QApplication& app)
{
    app.setStyle(new NoAltMenuStyle(QStyleFactory::create("Fusion")));

    loadBundledFonts();
    app.setWindowIcon(appIcon(monotone()));
    PrefStore& prefs = PrefStore::get();
    QFont ui(QString::fromStdString(prefs.value<std::string>("general", "/ui/font", "Inter")));
    ui.setPointSizeF(prefs.value<double>("general", "/ui/fontSize", 9.0));
    ui.setHintingPreference(QFont::PreferNoHinting);
    // Tabular figures, so numbers in fields don't shift width as they change.
    ui.setFeature(QFont::Tag("tnum"), 1);
    app.setFont(ui);

    // Strictly grayscale: no hue anywhere in the chrome, including highlights and links.
    auto gray = [](int v) { return QColor(v, v, v); };
    QPalette p;
    p.setColor(QPalette::Window, gray(48));
    p.setColor(QPalette::WindowText, gray(212));
    p.setColor(QPalette::Base, gray(36));
    p.setColor(QPalette::AlternateBase, gray(42));
    p.setColor(QPalette::ToolTipBase, gray(28));
    p.setColor(QPalette::ToolTipText, gray(220));
    p.setColor(QPalette::PlaceholderText, gray(110));
    p.setColor(QPalette::Text, gray(212));
    p.setColor(QPalette::Button, gray(58));
    p.setColor(QPalette::ButtonText, gray(212));
    p.setColor(QPalette::BrightText, gray(255));
    p.setColor(QPalette::Highlight, gray(105));
    p.setColor(QPalette::HighlightedText, gray(255));
    p.setColor(QPalette::Link, gray(235));
    p.setColor(QPalette::LinkVisited, gray(190));
    p.setColor(QPalette::Light, gray(78));
    p.setColor(QPalette::Midlight, gray(66));
    p.setColor(QPalette::Mid, gray(40));
    p.setColor(QPalette::Dark, gray(30));
    p.setColor(QPalette::Shadow, gray(18));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, gray(110));
    p.setColor(QPalette::Disabled, QPalette::Highlight, gray(70));
    app.setPalette(p);

    auto* titleBars = new DarkTitleBars(&app);
    app.installEventFilter(titleBars);
    QPointer<DarkTitleBars> bars(titleBars);
    PrefStore::get().onChanged([bars](const std::string& file) {
        if (file != "general")
            return;
        QTimer::singleShot(0, qApp, [bars] {
            if (bars)
                bars->setMonotone(monotone());
        });
    });

    app.setStyleSheet(R"(
        QToolTip { border: 1px solid #555; padding: 3px; }
        QDockWidget::title { background: #3a3a3a; padding: 4px 6px; }
        QMainWindow::separator { background: #2a2a2a; width: 3px; height: 3px; }
        QMainWindow::separator:hover { background: #6a6a6a; }
        QStatusBar { background: #2c2c2c; }
        QStatusBar QToolButton { border: none; padding: 1px 8px; }
        QStatusBar QToolButton:checked { background: #5a5a5a; }
    )");
}
} // namespace lila::ui
