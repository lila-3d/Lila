#include "ui/UiTools.h"

#include "core/Paths.h"
#include "mcp/McpServer.h"
#include "ops/Command.h"
#include "ui/UiCommon.h"

#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QThread>
#include <QTimer>

#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lila::ui
{
namespace
{
/** Window contents including native Vulkan children, even when covered by other windows. */
QImage captureWindow(QWidget* w)
{
#ifdef _WIN32
    HWND hwnd = HWND(w->winId());
    RECT r;
    GetWindowRect(hwnd, &r);
    int width = r.right - r.left, height = r.bottom - r.top;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(dc, bmp);
    PrintWindow(hwnd, dc, 2 /* PW_RENDERFULLCONTENT */);
    QImage img(width, height, QImage::Format_ARGB32);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    GetDIBits(dc, bmp, 0, height, img.bits(), &bi, DIB_RGB_COLORS);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return img.convertToFormat(QImage::Format_RGB32);
#else
    return w->grab().toImage();
#endif
}

/** Main window plus any floating Lila windows (marking menus, popups) composited at their screen positions. */
QImage captureAll()
{
    QWidget* main = mainWindow();
    QImage img = captureWindow(main);
    QPainter p(&img);
    QPoint origin = main->frameGeometry().topLeft();
    for (QWidget* w : QApplication::topLevelWidgets())
    {
        if (w == main || !w->isVisible())
            continue;
        QRect r = w->frameGeometry().translated(-origin);
        if (!r.intersects(img.rect()))
            continue;
        // Translucent overlays render through Qt directly; native PrintWindow loses their alpha.
        QImage layer = w->testAttribute(Qt::WA_TranslucentBackground) ? w->grab().toImage() : captureWindow(w);
        p.drawImage(r.topLeft(), layer);
    }
    return img;
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 10);
}
QWidget* viewportWidget()
{
    // With four views up, the focused one wins, then any visible one.
    QWidget* visible = nullptr;
    if (QWidget* mw = mainWindow())
        for (QWidget* c : mw->findChildren<QWidget*>())
            if (c->property("hotkeyContext").toString() == "viewport" && c->isVisible())
            {
                if (c->hasFocus())
                    return c;
                if (!visible)
                    visible = c;
            }
    return visible;
}

/**
 * "window": the main window with its floating windows. "viewport": the active view as shown. Otherwise a dock's object
 * name ("outliner", "channelBox"): a docked one is cropped out of the window capture, a floating one taken on its own.
 */
QImage captureTarget(const std::string& target)
{
    QWidget* main = mainWindow();
    if (!main)
        throw std::runtime_error("needs the Lila window (batch mode has none; captureView renders offscreen)");
    if (target.empty() || target == "window")
        return captureAll();
    QWidget* w = nullptr;
    if (target == "viewport")
        w = viewportWidget();
    else if (auto* dock = main->findChild<QDockWidget*>(QString::fromStdString(target)))
    {
        if (dock->isFloating() && dock->isVisible())
            return captureWindow(dock);
        w = dock;
    }
    if (!w || !w->isVisible())
        throw std::runtime_error("nothing shown called '" + target + "' (window, viewport, or a panel such as outliner)");
    QImage all = captureAll();
#ifdef _WIN32
    const QRect frame = main->frameGeometry();
#else
    const QRect frame(main->mapToGlobal(QPoint(0, 0)), main->size());
#endif
    // The capture is in device pixels, widget geometry in logical ones.
    const double scale = frame.width() > 0 ? double(all.width()) / frame.width() : 1.0;
    const QPoint at = w->mapToGlobal(QPoint(0, 0)) - frame.topLeft();
    QRect r(QPoint(int(std::lround(at.x() * scale)), int(std::lround(at.y() * scale))),
            QSize(int(std::lround(w->width() * scale)), int(std::lround(w->height() * scale))));
    r = r.intersected(all.rect());
    if (r.isEmpty())
        throw std::runtime_error("'" + target + "' is outside the window");
    return all.copy(r);
}

Qt::KeyboardModifiers parseMods(const mcp::json& e)
{
    Qt::KeyboardModifiers m;
    for (const auto& x : e.value("mods", mcp::json::array()))
    {
        std::string s = x.get<std::string>();
        if (s == "shift") m |= Qt::ShiftModifier;
        if (s == "ctrl") m |= Qt::ControlModifier;
        if (s == "alt") m |= Qt::AltModifier;
    }
    return m;
}

Qt::MouseButton parseButton(const std::string& b)
{
    return b == "right" ? Qt::RightButton : b == "middle" ? Qt::MiddleButton : Qt::LeftButton;
}
} // namespace

void registerScreenshotCommand()
{
    if (ops::find("screenshot"))
        return;
    ops::registerCommand({
        .name = "screenshot",
        .doc = "Save a PNG of Lila's UI as it is on screen: the whole window, the active viewport (HUD, gizmos and overlays "
               "included) or one panel. Needs the window; captureView renders the scene offscreen instead, in batch mode too. "
               "Returns the path.",
        .flags = {{"path", "p", ArgType::String, nullptr, "PNG file to write; Lila's config folder/screenshot.png when left out."},
                  {"target", "t", ArgType::String, "window",
                   "window (with its floating windows), viewport, or a panel's name such as outliner or channelBox."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string path = a.get<std::string>("path", "");
            if (path.empty())
                path = (paths::userConfigDir() / "screenshot.png").string();
            try
            {
                // A script that just changed the layout has not repainted yet; the native viewport would be captured stale.
                pump(100);
                if (!captureTarget(a.get<std::string>("target", "window")).save(QString::fromStdString(path), "PNG"))
                    throw std::runtime_error("cannot write " + path);
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("screenshot: ") + e.what());
            }
            return path;
        },
    });
}

void registerUiTools()
{
    registerScreenshotCommand();
    mcp::Server& s = mcp::Server::get();

    s.addTool({
        .name = "send_mouse",
        .description = "Mouse input to the viewport. `events`: [{\"type\": \"press\"|\"move\"|\"release\", \"x\": px, "
                       "\"y\": px, \"button\": \"left\"|\"middle\"|\"right\", \"mods\": [\"shift\",\"ctrl\",\"alt\"]}] in "
                       "viewport-local logical pixels. Returns the viewport size.",
        .inputSchema = {{"type", "object"}, {"properties", {{"events", {{"type", "array"}}}}}, {"required", {"events"}}},
        .run = [](const mcp::json& args) {
            QWidget* vp = viewportWidget();
            if (!vp)
                throw std::runtime_error("no viewport");
            Qt::MouseButtons held;
            for (const auto& e : args["events"])
            {
                QPointF local(e.value("x", 0.0), e.value("y", 0.0));
                QPointF global = vp->mapToGlobal(local);
                std::string type = e.value("type", "move");
                Qt::MouseButton b = parseButton(e.value("button", "left"));
                QEvent::Type t = type == "press" ? QEvent::MouseButtonPress : type == "release" ? QEvent::MouseButtonRelease : QEvent::MouseMove;
                if (t == QEvent::MouseButtonPress)
                    held |= b;
                if (t == QEvent::MouseButtonRelease)
                    held &= ~b;
                QMouseEvent ev(t, local, global, t == QEvent::MouseMove ? Qt::NoButton : b, held | (t == QEvent::MouseButtonRelease ? b : Qt::NoButton), parseMods(e));
                QWidget* target = QWidget::mouseGrabber() ? QWidget::mouseGrabber() : vp;
                if (target != vp)
                {
                    QPointF tl = target->mapFromGlobal(global);
                    QMouseEvent ev2(t, tl, global, ev.button(), ev.buttons(), ev.modifiers());
                    QApplication::sendEvent(target, &ev2);
                }
                else
                    QApplication::sendEvent(vp, &ev);
                pump(e.value("wait_ms", 16));
            }
            return std::to_string(vp->width()) + "x" + std::to_string(vp->height());
        },
    });

    s.addTool({
        .name = "capture_window",
        .description = "Screenshot of Lila's UI as shown, saved as PNG; returns the path. `target`: \"window\" (default: the main "
                       "window with its floating windows), \"viewport\" (the active view with its HUD and overlays) or a panel's "
                       "name (\"outliner\", \"channelBox\"). The `screenshot` command does the same from Python.",
        .inputSchema = {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}, {"target", {{"type", "string"}}}}}},
        .run = [](const mcp::json& args) {
            if (!mainWindow())
                throw std::runtime_error("no main window");
            pump(50);
            std::string path = args.value("path", (paths::userConfigDir() / "capture.png").string());
            if (!captureTarget(args.value("target", "window")).save(QString::fromStdString(path)))
                throw std::runtime_error("failed to save " + path);
            return path;
        },
    });

    s.addTool({
        .name = "send_keys",
        .description = "Press keys in Lila as if typed, through the real hotkey system. `keys` is a list like "
                       "[\"W\", \"Ctrl+Z\"]. `hold_ms` keeps each key down that long (to trigger hold actions such as "
                       "marking menus). `widget`: focus target, \"viewport\" (default) or \"focus\". `capture_path` saves a "
                       "window capture while the last key is still held.",
        .inputSchema = {{"type", "object"},
                        {"properties", {{"keys", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                                        {"hold_ms", {{"type", "integer"}}},
                                        {"widget", {{"type", "string"}}},
                                        {"capture_path", {{"type", "string"}}}}},
                        {"required", {"keys"}}},
        .run = [](const mcp::json& args) {
            QWidget* target = QApplication::focusWidget();
            if (args.value("widget", "viewport") == "viewport")
                if (QWidget* mw = mainWindow())
                    for (QWidget* c : mw->findChildren<QWidget*>())
                        if (c->property("hotkeyContext").toString() == "viewport" && c->isVisible())
                        {
                            c->setFocus();
                            target = c;
                        }
            if (!target)
                throw std::runtime_error("no focus widget");
            int hold = args.value("hold_ms", 0);
            for (const auto& k : args["keys"])
            {
                QKeyCombination combo = QKeySequence::fromString(QString::fromStdString(k.get<std::string>()))[0];
                QKeyEvent press(QEvent::KeyPress, combo.key(), combo.keyboardModifiers());
                QApplication::sendEvent(QWidget::keyboardGrabber() ? QWidget::keyboardGrabber() : target, &press);
                pump(hold);
                if (args.contains("capture_path"))
                    captureAll().save(QString::fromStdString(args["capture_path"].get<std::string>()));
                QKeyEvent release(QEvent::KeyRelease, combo.key(), combo.keyboardModifiers());
                QApplication::sendEvent(QWidget::keyboardGrabber() ? QWidget::keyboardGrabber() : target, &release);
                pump(30);
            }
            return std::string("ok");
        },
    });
}
} // namespace lila::ui
