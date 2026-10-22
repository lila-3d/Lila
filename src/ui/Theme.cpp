#include "ui/Theme.h"

#include "core/Paths.h"
#include "core/PrefStore.h"

#include <QApplication>
#include <QFontDatabase>
#include <QIcon>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QWidget>

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

/**
 * Gives every top-level window (dialogs, floating docks, tool windows) the dark native title bar, and the grey icon in
 * it. The taskbar keeps the colour icon (the window's big icon).
 */
class DarkTitleBars : public QObject
{
public:
    using QObject::QObject;

#ifdef _WIN32
    ~DarkTitleBars() override
    {
        if (mSmallIcon)
            DestroyIcon(mSmallIcon);
    }
#endif

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
#ifdef _WIN32
        if (e->type() == QEvent::Show || e->type() == QEvent::WinIdChange)
            if (auto* w = qobject_cast<QWidget*>(obj); w && w->isWindow() && w->testAttribute(Qt::WA_WState_Created))
            {
                BOOL dark = TRUE;
                HWND hwnd = HWND(w->winId());
                DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
                if (HICON icon = smallIcon())
                    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, LPARAM(icon));
            }
#else
        (void)e;
        (void)obj;
#endif
        return false;
    }

private:
#ifdef _WIN32
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

    HICON mSmallIcon = nullptr;
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
    app.setWindowIcon(QIcon(QString::fromStdWString((paths::resourcesDir() / "icons" / "lila_color.png").wstring())));
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

    app.installEventFilter(new DarkTitleBars(&app));

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
