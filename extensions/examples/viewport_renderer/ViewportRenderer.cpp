// A viewport renderer through the renderer override: a CPU rasteriser that shades the scene's meshes by their normals
// on a worker thread and delivers CPU pixels and depth, first at a quarter of the size, then in full. Lila draws its
// wires, selection, gizmos and HUD over the result. Pick it from a viewport's Renderer menu or with
// viewportRenderer("cpuShaded").

#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Transform.h>
#include <lila/ViewportRenderers.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using namespace lila;
using namespace lila::ui;

namespace
{
/** The scene as the worker sees it: each visible mesh with its world matrix, read on the main thread. */
struct MeshCopy
{
    scene::MeshPtr mesh;
    glm::dmat4 world{1.0};
};

struct Job
{
    viewportRenderers::Request request;
    std::vector<MeshCopy> meshes;
};

std::vector<MeshCopy> visibleMeshes(scene::Graph& g)
{
    std::vector<MeshCopy> out;
    g.evaluate();
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh") || !scene::effectivelyVisible(g, id))
            continue;
        if (const scene::MeshPtr& m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh); m && !m->empty())
            out.push_back({m, n->as<glm::dmat4>(scene::Transform::kWorldMatrix)});
    }
    return out;
}

/** Rasterises job at width x height: normal-shaded colour where a mesh covers a pixel, depth along the view axis. */
viewportRenderers::Frame rasterise(const Job& job, int width, int height, const std::atomic<bool>& cancel)
{
    const viewportRenderers::Request& r = job.request;
    viewportRenderers::Frame f;
    f.width = width;
    f.height = height;
    f.serial = r.serial;
    f.rgba8.assign(size_t(width) * height * 4, 0);
    f.depth.assign(size_t(width) * height, 0.0f);
    std::vector<float> nearest(size_t(width) * height, std::numeric_limits<float>::max());
    const bool ortho = r.view.ortho;
    const double nearZ = -std::max(r.nearClip, 1e-4);

    auto toScreen = [&](const glm::dvec3& v) {
        const glm::dvec4 c = r.view.proj * glm::dvec4(v, 1.0);
        return glm::dvec3((c.x / c.w * 0.5 + 0.5) * width, (c.y / c.w * 0.5 + 0.5) * height, -v.z);
    };

    auto drawTriangle = [&](const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, const uint8_t (&rgb)[3]) {
        glm::dvec3 p[3] = {toScreen(a), toScreen(b), toScreen(c)};
        const double area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x);
        if (std::abs(area) < 1e-12)
            return;
        const int x0 = std::max(0, int(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
        const int x1 = std::min(width - 1, int(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
        const int y0 = std::max(0, int(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
        const int y1 = std::min(height - 1, int(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const double px = x + 0.5, py = y + 0.5;
                double w0 = ((p[1].x - px) * (p[2].y - py) - (p[1].y - py) * (p[2].x - px)) / area;
                double w1 = ((p[2].x - px) * (p[0].y - py) - (p[2].y - py) * (p[0].x - px)) / area;
                double w2 = 1.0 - w0 - w1;
                if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0)
                    continue;
                // Perspective depth interpolates as its inverse across the screen, orthographic as itself.
                const double d = ortho ? w0 * p[0].z + w1 * p[1].z + w2 * p[2].z : 1.0 / (w0 / p[0].z + w1 / p[1].z + w2 / p[2].z);
                const size_t i = size_t(y) * width + x;
                if (d <= 0.0 || d >= nearest[i])
                    continue;
                nearest[i] = float(d);
                f.depth[i] = float(d);
                f.rgba8[i * 4 + 0] = rgb[0];
                f.rgba8[i * 4 + 1] = rgb[1];
                f.rgba8[i * 4 + 2] = rgb[2];
                f.rgba8[i * 4 + 3] = 255;
            }
    };

    // Clips a polygon to the near plane before drawing it as a fan, so faces passing the camera keep their visible part.
    auto drawPolygon = [&](std::vector<glm::dvec3>& poly, const uint8_t (&rgb)[3]) {
        if (!ortho)
        {
            std::vector<glm::dvec3> kept;
            for (size_t i = 0; i < poly.size(); ++i)
            {
                const glm::dvec3 &a = poly[i], &b = poly[(i + 1) % poly.size()];
                const bool ina = a.z <= nearZ, inb = b.z <= nearZ;
                if (ina)
                    kept.push_back(a);
                if (ina != inb)
                    kept.push_back(a + (b - a) * ((nearZ - a.z) / (b.z - a.z)));
            }
            poly.swap(kept);
        }
        for (size_t i = 2; i < poly.size(); ++i)
            drawTriangle(poly[0], poly[i - 1], poly[i], rgb);
    };

    std::vector<glm::dvec3> poly;
    for (const MeshCopy& m : job.meshes)
    {
        const glm::dmat4 toView = r.view.view * m.world;
        const glm::dmat3 normalToWorld = glm::transpose(glm::inverse(glm::dmat3(m.world)));
        const glm::dmat3 worldToView(r.view.view);
        const geo::Mesh& mesh = *m.mesh;
        for (geo::Index face = 0; face < mesh.faceCapacity(); ++face)
        {
            if (!mesh.faceValid(face))
                continue;
            if (cancel.load(std::memory_order_relaxed))
                return f;
            poly.clear();
            mesh.forEachFaceCorner(face, [&](geo::Index c) { poly.push_back(glm::dvec3(toView * glm::dvec4(mesh.position(mesh.cornerPoint(c)), 1.0))); });
            // Tinted by the world normal, darker as the face turns away from the eye.
            const glm::dvec3 normal = glm::normalize(normalToWorld * mesh.faceNormal(face));
            glm::dvec3 centre(0.0);
            for (const glm::dvec3& v : poly)
                centre += v / double(poly.size());
            const double facing = std::abs(glm::dot(worldToView * normal, ortho ? glm::dvec3(0, 0, 1) : glm::normalize(-centre)));
            const glm::dvec3 shade = glm::clamp((0.5 + 0.5 * normal) * (0.25 + 0.75 * facing), 0.0, 1.0) * 255.0 + 0.5;
            const uint8_t rgb[3] = {uint8_t(shade.x), uint8_t(shade.y), uint8_t(shade.z)};
            drawPolygon(poly, rgb);
        }
    }
    return f;
}

/**
 * One viewport's renderer: the newest request waits for a worker thread, which delivers a quarter-size preview and then
 * the full frame. A newer request cancels the frame under way.
 */
class CpuSession final : public viewportRenderers::Session
{
public:
    CpuSession() : mWorker([this] { run(); }) {}

    ~CpuSession() override
    {
        {
            std::scoped_lock lock(mMutex);
            mQuit = true;
            mCancel = true;
        }
        mWake.notify_all();
        mWorker.join();
    }

    void render(const viewportRenderers::Request& r) override
    {
        auto job = std::make_shared<Job>();
        job->request = r;
        job->request.graph = nullptr;
        job->meshes = visibleMeshes(*r.graph);
        {
            std::scoped_lock lock(mMutex);
            mJob = std::move(job);
            mCancel = true;
        }
        mWake.notify_all();
    }

private:
    void run()
    {
        while (true)
        {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mMutex);
                mWake.wait(lock, [this] { return mQuit || mJob; });
                if (mQuit)
                    return;
                job = std::move(mJob);
                mCancel = false;
            }
            const viewportRenderers::Request& r = job->request;
            for (int divisor : {4, 1})
            {
                const int w = std::max(1, r.width / divisor), h = std::max(1, r.height / divisor);
                viewportRenderers::Frame frame = rasterise(*job, w, h, mCancel);
                if (mCancel)
                    break;
                r.deliver(std::move(frame));
            }
        }
    }

    std::mutex mMutex;
    std::condition_variable mWake;
    std::shared_ptr<Job> mJob;
    std::atomic<bool> mCancel{false};
    bool mQuit = false;
    std::thread mWorker;
};
} // namespace

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    const viewportRenderers::Renderer* r =
        viewportRenderers::add(ctx, {.id = "cpuShaded",
                                     .name = "CPU Normals (example)",
                                     .doc = "Example renderer: the scene's meshes rasterised on the CPU and shaded by their normals.",
                                     .open = [](int) { return std::make_unique<CpuSession>(); }});
    return r ? 0 : 1;
}
