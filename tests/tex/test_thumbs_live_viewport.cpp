#include "TexTest.h"

#include "core/MainQueue.h"
#include "ops/Command.h"
#include "render/Renderer.h"
#include "tex/Library.h"
#include "ui/Environments.h"
#include "ui/MaterialThumbnails.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace textest;
namespace lib = lila::tex::library;
namespace thumbs = lila::ui::materialThumbs;

#ifdef _WIN32
namespace
{
/** A native window this thread owns but never shows, so the swapchain's driver messages land on a thread we control. */
HWND hiddenWindow()
{
    static const wchar_t* cls = L"LilaThumbTestWindow";
    static bool registered = false;
    if (!std::exchange(registered, true))
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        RegisterClassW(&wc);
    }
    // Shown off screen, without activation or a taskbar button, so its swapchain is real but nobody sees it.
    HWND top = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, L"thumb test", WS_POPUP, -20000, -20000, 320, 240, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    ShowWindow(top, SW_SHOWNOACTIVATE);
    return top;
}

void pumpWindows()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
} // namespace

TEST_CASE("material thumbnails render asynchronously while a live viewport presents, never blocking the main thread")
{
    render::Renderer& r = render::Renderer::get();
    if (!r.running() && !r.start())
    {
        MESSAGE("no GPU");
        return;
    }
    if (!ops::find("materialLibrary"))
        lib::registerCommands();
    ops::execute("newScene", json::object());

    Graph g = Graph::makeMaterial("Blue");
    g.nodes.front().values = {{"baseColor", {0.02, 0.05, 0.6, 1.0}}, {"roughness", 0.4}};
    std::filesystem::path file = lib::tierDir("user") / "LiveTest" / "blue.lilamat";
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << json{{"format", "lilamat"}, {"version", 1}, {"name", "Blue"}, {"graph", g.toJson()}}.dump();
    lib::reload();
    std::filesystem::remove_all(thumbs::cacheDir());

    HWND hwnd = hiddenWindow();
    REQUIRE(hwnd);
    render::ViewportId vp = r.addViewport(uintptr_t(hwnd), 320, 240);
    // The viewport lights with the default HDRI, as the app's views do.
    render::FrameState s;
    s.envPath = ui::environments::resolve("");
    r.submit(vp, s);

    // The main thread keeps its window serviced (as Qt's loop does) and the thumbnail arrives through the main queue.
    bool done = false;
    thumbs::Result got;
    std::string error;
    thumbs::Options o;
    o.size = 64;
    auto t0 = std::chrono::steady_clock::now();
    thumbs::renderAsync("LiveTest/blue", o, [&](const thumbs::Result& res, const std::string& err) {
        got = res;
        error = err;
        done = true;
    });
    int frames = 0;
    while (!done && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30))
    {
        pumpWindows();
        mainq::drain();
        // Keep the viewport presenting, as an orbiting user would.
        if (++frames % 4 == 0)
            r.submit(vp, s);
        Sleep(5);
    }
    INFO(error);
    CHECK(done);
    CHECK(error.empty());
    CHECK(std::filesystem::exists(got.file));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("async thumbnail with a live viewport: ", ms, " ms");

    // The blocking form, called on the window's thread, pumps messages while it waits instead of deadlocking.
    o.force = true;
    t0 = std::chrono::steady_clock::now();
    thumbs::Result sync = thumbs::render("LiveTest/blue", o);
    CHECK(std::filesystem::exists(sync.file));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));

    r.removeViewport(vp);
    DestroyWindow(hwnd);
    std::filesystem::remove_all(file.parent_path());
    lib::reload();
}
#endif
