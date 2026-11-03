#include <doctest/doctest.h>

#include "render/Renderer.h"
#include "ui/Camera.h"

#include <chrono>
#include <utility>

#ifdef _WIN32
#include <windows.h>

using namespace lila;
using namespace lila::render;

namespace
{
HWND offscreenWindow()
{
    static const wchar_t* cls = L"LilaResidencyTestWindow";
    static bool registered = false;
    if (!std::exchange(registered, true))
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        RegisterClassW(&wc);
    }
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, L"residency test", WS_POPUP, -20000, -20000, 320, 240, nullptr,
                             nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(w, SW_SHOWNOACTIVATE);
    return w;
}

void pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

std::shared_ptr<RenderMesh> quad()
{
    auto m = std::make_shared<RenderMesh>();
    for (int c = 0; c < 4; ++c)
    {
        MeshVertex v{};
        v.pos = glm::vec3((c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f, 0.0f);
        v.normal = glm::vec3(0.0f, 0.0f, 1.0f);
        v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        m->vertices.push_back(v);
    }
    m->indices = {0, 1, 3, 0, 3, 2};
    m->boundsMin = glm::vec3(-1.0f, -1.0f, 0.0f);
    m->boundsMax = glm::vec3(1.0f, 1.0f, 0.0f);
    return m;
}

/** The same draw list seen from a camera turned by yaw degrees. */
FrameState frame(const std::shared_ptr<RenderMesh>& mesh, double yaw)
{
    FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.msaaSamples = 1;
    s.lighting = FrameState::Lighting::Studio;
    ui::Camera cam;
    cam.frame(glm::dvec3(0.0), 2.0, 320.0 / 240.0);
    cam.yaw += yaw;
    s.view = cam.view();
    s.proj = cam.proj(320.0 / 240.0);
    s.eye = cam.eye();
    MeshDraw d;
    d.mesh = mesh;
    d.flags = DrawShaded;
    s.meshes.push_back(d);
    return s;
}

/** Submits frames until the view has drawn `count` more, or a timeout. */
uint64_t drawFrames(Renderer& r, ViewportId vp, const std::shared_ptr<RenderMesh>& mesh, int count, double& yaw)
{
    const uint64_t target = r.stats(vp).frames + uint64_t(count);
    const auto t0 = std::chrono::steady_clock::now();
    while (r.stats(vp).frames < target && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60))
    {
        yaw += 0.5;
        r.submit(vp, frame(mesh, yaw));
        pump();
        Sleep(1);
    }
    return r.stats(vp).frames;
}
} // namespace

TEST_CASE("a mesh the view keeps drawing stays on the GPU while only the camera moves")
{
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
    {
        MESSAGE("no GPU");
        return;
    }
    HWND hwnd = offscreenWindow();
    REQUIRE(hwnd);
    const ViewportId vp = r.addViewport(uintptr_t(hwnd), 320, 240);
    const std::shared_ptr<RenderMesh> mesh = quad();
    double yaw = 0.0;

    const uint64_t warm = drawFrames(r, vp, mesh, 10, yaw);
    const uint64_t uploads = r.stats(vp).meshUploads;
    CHECK(uploads > 0);
    // Past the eviction sweep's 240-frame window: a mesh still drawn must not be freed and copied again.
    const uint64_t drawn = drawFrames(r, vp, mesh, 600, yaw);
    CHECK(drawn - warm >= 600);
    CHECK(r.stats(vp).meshUploads == uploads);

    r.removeViewport(vp);
    DestroyWindow(hwnd);
}
#endif
