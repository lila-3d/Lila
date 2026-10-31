// Hydra viewport renderers: each viewport session owns a render index for one render delegate, fed by Lila's scene
// index, its materials and the live stages (UsdHydraScene.h), drawn through HdxTaskController and read back from its
// color and depth AOVs as CPU pixels. Storm runs on Vulkan through its own device, headless.

#include "UsdBackend.h"
#include "UsdCommon.h"
#include "UsdHydraScene.h"
#include "UsdStage.h"

#include <lila/Log.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Tasks.h>
#include <lila/Transform.h>
#include <lila/ViewportRenderers.h>

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/glf/simpleLightingContext.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/rendererCreateArgsSchema.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/colorCorrectionTask.h>
#include <pxr/imaging/hdx/renderSetupTask.h>
#include <pxr/imaging/hdx/selectionTracker.h>
#include <pxr/imaging/hdx/taskController.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/usdImaging/usdImaging/sceneIndex.h>

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace lila;

namespace
{
namespace vr = ui::viewportRenderers;
using namespace usd::hydra;
using scene::Graph;
using scene::NodeId;

/** One Vulkan Hgi for every session, kept for the process: Storm's shared resources outlive any one viewport. */
Hgi* sharedHgi(std::string& error)
{
    static Hgi* hgi = nullptr;
    static std::string failure;
    static std::once_flag once;
    std::call_once(once, [] {
        HgiUniquePtr h = Hgi::CreateNamedHgi(HgiTokens->Vulkan);
        if (!h)
            failure = "HgiVulkan is not available";
        else if (!h->IsBackendSupported())
            failure = "no Vulkan device Hydra can use";
        else
            hgi = h.release();
    });
    if (!hgi)
        error = failure;
    return hgi;
}

/**
 * Lila's projection as Hydra wants it: OpenGL clip space (y up, depth -1 to 1, not reversed) with a finite far plane.
 * The x and y rows are Lila's, so the framing matches the viewport exactly.
 */
glm::dmat4 glProjection(const vr::Request& r)
{
    const glm::dmat4& p = r.view.proj;
    const double n = r.view.ortho ? r.nearClip : std::max(r.nearClip, 1e-4);
    const double f = r.farClip > n ? r.farClip : std::max(std::abs(n) * 1e7, 1e6);
    glm::dmat4 g(0.0);
    for (int c = 0; c < 4; ++c)
    {
        g[c][0] = p[c][0];
        g[c][1] = -p[c][1];
    }
    if (r.view.ortho)
    {
        g[2][2] = -2.0 / (f - n);
        g[3][2] = -(f + n) / (f - n);
        g[3][3] = 1.0;
    }
    else
    {
        g[2][2] = -(f + n) / (f - n);
        g[3][2] = -2.0 * f * n / (f - n);
        g[2][3] = -1.0;
    }
    return g;
}

/** Calls fn with a reader of format's components (pixel, index), chosen once so pixel loops don't switch per component. */
template <typename Fn>
void withReader(HdFormat format, Fn&& fn)
{
    switch (HdGetComponentFormat(format))
    {
    case HdFormatUNorm8: fn([](const uint8_t* p, int i) { return p[i] * (1.0f / 255.0f); }); break;
    case HdFormatSNorm8: fn([](const uint8_t* p, int i) { return std::max(-1.0f, int8_t(p[i]) / 127.0f); }); break;
    case HdFormatFloat16: fn([](const uint8_t* p, int i) { return float(reinterpret_cast<const GfHalf*>(p)[i]); }); break;
    case HdFormatFloat32: fn([](const uint8_t* p, int i) { return reinterpret_cast<const float*>(p)[i]; }); break;
    case HdFormatInt32: fn([](const uint8_t* p, int i) { return float(reinterpret_cast<const int32_t*>(p)[i]); }); break;
    default: break;
    }
}

/** Runs fn(y) for each of h rows across Lila's thread pool. */
template <typename Fn>
void forRows(int h, Fn&& fn)
{
    tbb::parallel_for(tbb::blocked_range<int>(0, h, 16), [&](const tbb::blocked_range<int>& rows) {
        for (int y = rows.begin(); y < rows.end(); ++y)
            fn(y);
    });
}

/** A render setting's value as Lila's settings carry it: bool, number or string; null for any other type. */
nlohmann::json toJson(const VtValue& v)
{
    if (v.IsHolding<bool>())
        return v.UncheckedGet<bool>();
    if (v.IsHolding<int>())
        return v.UncheckedGet<int>();
    if (v.IsHolding<unsigned int>())
        return v.UncheckedGet<unsigned int>();
    if (v.IsHolding<int64_t>())
        return v.UncheckedGet<int64_t>();
    if (v.IsHolding<float>())
        return v.UncheckedGet<float>();
    if (v.IsHolding<double>())
        return v.UncheckedGet<double>();
    if (v.IsHolding<std::string>())
        return v.UncheckedGet<std::string>();
    if (v.IsHolding<TfToken>())
        return v.UncheckedGet<TfToken>().GetString();
    return nullptr;
}

/** A value for a render setting, as the type of like (its current value) when there is one; empty when it doesn't convert. */
VtValue toVt(const nlohmann::json& j, const VtValue& like = VtValue())
{
    if (!like.IsEmpty())
    {
        if (like.IsHolding<bool>() && j.is_boolean())
            return VtValue(j.get<bool>());
        if (j.is_number())
        {
            if (like.IsHolding<int>())
                return VtValue(j.get<int>());
            if (like.IsHolding<unsigned int>())
                return VtValue(j.get<unsigned int>());
            if (like.IsHolding<int64_t>())
                return VtValue(j.get<int64_t>());
            if (like.IsHolding<float>())
                return VtValue(j.get<float>());
            if (like.IsHolding<double>())
                return VtValue(j.get<double>());
        }
        if (j.is_string() && like.IsHolding<TfToken>())
            return VtValue(TfToken(j.get<std::string>()));
        if (j.is_string() && like.IsHolding<std::string>())
            return VtValue(j.get<std::string>());
        return VtValue();
    }
    if (j.is_boolean())
        return VtValue(j.get<bool>());
    if (j.is_number_integer())
        return VtValue(j.get<int>());
    if (j.is_number())
        return VtValue(j.get<double>());
    if (j.is_string())
        return VtValue(j.get<std::string>());
    return VtValue();
}

struct StageChain
{
    UsdStageRefPtr stage;
    UsdImagingSceneIndexRefPtr usd;
    TfRefPtr<StageOverrides> overrides;
    TfRefPtr<StagePlacement> world;
};

class HydraSession final : public vr::Session
{
public:
    HydraSession(const std::string& plugin, int panel, const nlohmann::json& saved, std::string& error)
    {
        HdRendererPluginRegistry& registry = HdRendererPluginRegistry::GetInstance();
        HdRendererPluginHandle handle = registry.GetOrCreateRendererPlugin(TfToken(plugin));
        if (!handle)
        {
            error = "no render delegate " + plugin + " on the plugin path";
            return;
        }
        std::string hgiError;
        Hgi* hgi = sharedHgi(hgiError);
        HdContainerDataSourceHandle drivers;
        if (hgi)
        {
            const TfToken name = HgiTokens->renderDriver;
            const HdDataSourceBaseHandle value = HdRetainedTypedSampledDataSource<Hgi*>::New(hgi);
            drivers = HdRetainedContainerDataSource::New(1, &name, &value);
        }
        const HdRendererCreateArgsSchema args(
            HdRendererCreateArgsSchema::Builder().SetGpuEnabled(HdRetainedTypedSampledDataSource<bool>::New(true)).SetDrivers(drivers).Build());
        mStorm = plugin == "HdStormRendererPlugin";
        std::string why;
        if (mStorm && !hgi)
        {
            error = hgiError;
            return;
        }
        // Storm's own check tries the platform's default Hgi (OpenGL on Windows), which needs a window; it runs on ours.
        if (!mStorm && !handle->IsSupported(args, &why))
        {
            error = why.empty() ? "it can't run here" : why;
            return;
        }
        // Lila's scene units and up axis, for delegates placing it in their own world, with the settings saved for it.
        HdRenderSettingsMap initial;
        initial[TfToken("lila:metersPerUnit")] = VtValue(0.01);
        initial[TfToken("lila:upAxis")] = VtValue(std::string(PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "y" ? "Y" : "Z"));
        for (auto& [key, value] : saved.items())
            if (VtValue v = toVt(value); !v.IsEmpty())
                initial[TfToken(key)] = v;
        mDelegate = handle->CreateDelegate(initial);
        if (!mDelegate)
        {
            error = "it failed to start";
            return;
        }
        HdDriverVector driverList;
        if (hgi)
        {
            mDriver.name = HgiTokens->renderDriver;
            mDriver.driver = VtValue(hgi);
            driverList.push_back(&mDriver);
        }
        mIndex.reset(HdRenderIndex::New(mDelegate.Get(), driverList, std::format("lilaViewport{}", panel), "Lila"));
        if (!mIndex)
        {
            error = "its render index failed to start";
            return;
        }
        mTasks = std::make_unique<HdxTaskController>(mIndex.get(), SdfPath(std::format("/LilaTasks/Panel{}", panel)), true);

        TfTokenVector aovs = {HdAovTokens->color};
        if (mDelegate->GetDefaultAovDescriptor(HdAovTokens->depth).format != HdFormatInvalid)
            aovs.push_back(HdAovTokens->depth);
        mTasks->SetRenderOutputs(aovs);
        HdAovDescriptor color = mTasks->GetRenderOutputSettings(HdAovTokens->color);
        color.clearValue = VtValue(GfVec4f(0.0f));
        mTasks->SetRenderOutputSettings(HdAovTokens->color, color);
        mTasks->SetEnableSelection(false);
        mTasks->SetEnablePresentation(false);
        mTasks->SetRenderTags({HdRenderTagTokens->geometry, HdRenderTagTokens->render});
        mTasks->SetCollection(HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        HdxColorCorrectionTaskParams cc;
        cc.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        mTasks->SetColorCorrectionParams(cc);
        HdxRenderTaskParams params;
        params.enableLighting = true;
        params.enableSceneLights = true;
        mTasks->SetRenderParams(params);
        mEngine.SetTaskContextData(HdxTokens->selectionState, VtValue(std::make_shared<HdxSelectionTracker>()));

        // A light at the eye for scenes that have none, as Lila's default lighting and usdview's camera light do.
        GlfSimpleLight eye;
        eye.SetIsCameraSpaceLight(true);
        eye.SetPosition(GfVec4f(0.0f, 0.0f, 0.0f, 1.0f));
        eye.SetDiffuse(GfVec4f(1.0f));
        eye.SetSpecular(GfVec4f(0.2f));
        eye.SetAmbient(GfVec4f(0.0f));
        GlfSimpleMaterial material;
        material.SetAmbient(GfVec4f(0.2f, 0.2f, 0.2f, 1.0f));
        material.SetSpecular(GfVec4f(0.1f, 0.1f, 0.1f, 1.0f));
        material.SetShininess(32.0);
        for (bool withEye : {true, false})
        {
            GlfSimpleLightingContextRefPtr ctx = GlfSimpleLightingContext::New();
            ctx->SetLights(withEye ? GlfSimpleLightVector{eye} : GlfSimpleLightVector{});
            ctx->SetMaterial(material);
            ctx->SetSceneAmbient(GfVec4f(0.01f, 0.01f, 0.01f, 1.0f));
            ctx->SetUseLighting(true);
            (withEye ? mEyeLight : mSceneLights) = ctx;
        }

        mLila = LilaSceneIndex::New();
        mIndex->InsertSceneIndex(mLila, SdfPath::AbsoluteRootPath(), false);
        mMaterialIndex = UsdImagingSceneIndex::New(HdRetainedContainerDataSource::New(), [](const HdSceneIndexBaseRefPtr& in) { return in; });
        mMaterialIndex->SetStage(mMaterials.stage());
        mIndex->InsertSceneIndex(mMaterialIndex, materialsRoot());
        mOk = true;
    }

    ~HydraSession() override
    {
        *mAlive = false;
        if (mIndex)
        {
            for (auto& [node, c] : mStages)
                mIndex->RemoveSceneIndex(c.world);
            mIndex->RemoveSceneIndex(mMaterialIndex);
            mIndex->RemoveSceneIndex(mLila);
        }
        mTasks.reset();
        mIndex.reset();
    
    }

    bool ok() const { return mOk; }

    void render(const vr::Request& r) override
    {
        LILA_ZONE();
        if (!r.graph || r.width <= 0 || r.height <= 0)
            return;
        mLast = r;
        Graph& g = *r.graph;
        g.evaluate();
        ViewContext view;
        view.isolation = r.view.isolation;
        // Request::view measures logical pixels, the frame device ones.
        const double device = r.view.width > 0.0 ? r.width / r.view.width : 1.0;
        view.unitsPerPixel = [&r, device](const glm::dvec3& at) { return r.view.unitsPerPixel(at) / device; };
        view.curvePixels = float(2.0 * device);
        view.curveColor = ui::themeLinear("curve", view.curveColor);
        mLila->sync(g, view, [this](const std::string& slot) { return mMaterials.bind(slot); });
        mMaterials.sync(g, mLila->usedSlots());
        mMaterialIndex->ApplyPendingUpdates();
        syncStages(g, view, r.time);
        draw();
    }

    void sceneChanged(scene::Change c, NodeId id) override { mMaterials.changed(Graph::current(), c, id); }

    std::vector<vr::Setting> settings() const override
    {
        std::vector<vr::Setting> out;
        for (const HdRenderSettingDescriptor& d : mDelegate->GetRenderSettingDescriptors())
        {
            VtValue v = mDelegate->GetRenderSetting(d.key);
            nlohmann::json j = toJson(v.IsEmpty() ? d.defaultValue : v);
            if (!j.is_null())
                out.push_back({d.key.GetString(), d.name, std::move(j)});
        }
        return out;
    }

    bool setSetting(const std::string& key, const nlohmann::json& value) override
    {
        const TfToken k(key);
        VtValue current = mDelegate->GetRenderSetting(k);
        if (current.IsEmpty())
            for (const HdRenderSettingDescriptor& d : mDelegate->GetRenderSettingDescriptors())
                if (d.key == k)
                    current = d.defaultValue;
        VtValue v = toVt(value, current);
        if (v.IsEmpty())
            return false;
        mDelegate->SetRenderSetting(k, v);
        return true;
    }

    /** For tests: what the last sync changed among Lila's prims. */
    LilaSceneIndex::Counts lastSync() const { return mLila->lastSync(); }

private:
    void syncStages(Graph& g, const ViewContext& view, double time)
    {
        std::set<NodeId> live;
        for (const usd::stage::HydraView& v : usd::stage::hydraViews(g))
        {
            if (!scene::effectivelyVisible(g, v.node))
                continue;
            live.insert(v.node);
            StageChain& c = mStages[v.node];
            if (!c.usd || c.stage != v.stage)
            {
                if (c.world)
                    mIndex->RemoveSceneIndex(c.world);
                c.stage = v.stage;
                TfRefPtr<StageOverrides> overrides;
                c.usd = UsdImagingSceneIndex::New(HdRetainedContainerDataSource::New(), [&overrides](const HdSceneIndexBaseRefPtr& in) {
                    overrides = StageOverrides::New(in);
                    return HdSceneIndexBaseRefPtr(overrides);
                });
                c.overrides = overrides;
                c.usd->SetStage(v.stage);
                c.world = StagePlacement::New(c.usd);
                mIndex->InsertSceneIndex(c.world, stageRoot(v.node));
            }
            c.usd->SetTime(UsdTimeCode(time * v.timeCodesPerSecond));
            c.usd->ApplyPendingUpdates();

            const glm::dmat4 fromWorld = glm::inverse(v.toWorld);
            std::map<SdfPath, StagePlacement::Placed> placed;
            std::set<SdfPath> blank;
            for (const usd::stage::HydraView::Link& l : v.links)
                if (const scene::Node* n = g.node(l.node))
                {
                    placed[l.prim] = {.matrix = fromWorld * n->as<glm::dmat4>(scene::Transform::kWorldMatrix) * (l.turned ? v.turnedToLila : v.toLila),
                                      .visible = scene::effectivelyVisible(g, l.node) && (!view.isolation || view.isolation->shows(l.node))};
                    if (drawnByLila(*n))
                        blank.insert(l.prim);
                }
            if (c.overrides)
                c.overrides->update(std::move(blank), std::set<SdfPath>(v.realized.begin(), v.realized.end()));
            c.world->update(v.toWorld, std::move(placed));
        }
        for (auto it = mStages.begin(); it != mStages.end();)
        {
            if (live.count(it->first))
            {
                ++it;
                continue;
            }
            mIndex->RemoveSceneIndex(it->second.world);
            it = mStages.erase(it);
        }
    }

    bool sceneHasLights() const
    {
        for (const TfToken& type : HdLightPrimTypeTokens())
            if (type != HdPrimTypeTokens->simpleLight && mIndex->IsSprimTypeSupported(type))
                for (const SdfPath& p : mIndex->GetSprimSubtree(type, SdfPath::AbsoluteRootPath()))
                    if (!p.HasPrefix(SdfPath("/LilaTasks")))
                        return true;
        return false;
    }

    void draw()
    {
        LILA_ZONE();
        const vr::Request& r = mLast;
        const glm::dmat4 proj = glProjection(r);
        mTasks->SetRenderBufferSize(GfVec2i(r.width, r.height));
        mTasks->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), r.width, r.height)));
        mTasks->SetFreeCameraMatrices(usd::toGf(r.view.view), usd::toGf(proj));
        mTasks->SetLightingState(sceneHasLights() ? mSceneLights : mEyeLight);
        HdTaskSharedPtrVector tasks = mTasks->GetRenderingTasks();
        mEngine.Execute(mIndex.get(), &tasks);
        if (r.deliver)
            r.deliver(readBack(r, proj));
        if (!mTasks->IsConverged())
            refineLater(r.serial);
    }

    /** Progressive delegates: draw again a moment later, until they converge or a newer request comes. */
    void refineLater(uint64_t serial)
    {
        std::weak_ptr<bool> alive = mAlive;
        std::thread([alive, serial, this] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            mainq::post([alive, serial, this] {
                if (const auto a = alive.lock(); a && *a && mLast.serial == serial)
                    draw();
            });
        }).detach();
    }

    vr::Frame readBack(const vr::Request& r, const glm::dmat4& proj)
    {
        LILA_ZONE();
        vr::Frame f;
        f.serial = r.serial;
        HdRenderBuffer* color = mTasks->GetRenderOutput(HdAovTokens->color);
        if (!color)
            return f;
        color->Resolve();
        const int w = int(color->GetWidth()), h = int(color->GetHeight());
        const HdFormat cf = color->GetFormat();
        const uint8_t* data = static_cast<const uint8_t*>(color->Map());
        if (!data || w <= 0 || h <= 0)
        {
            color->Unmap();
            return f;
        }
        f.width = w;
        f.height = h;
        f.rgba32f.resize(size_t(w) * h * 4);
        const size_t stride = HdDataSizeOfFormat(cf);
        const int components = int(HdGetComponentCount(cf));
        const float exposure = r.exposure;
        withReader(cf, [&](auto read) {
            // Hydra's buffers start at the bottom row; Lila's frames at the top.
            forRows(h, [&](int y) {
                const uint8_t* p = data + size_t(h - 1 - y) * w * stride;
                float* o = &f.rgba32f[size_t(y) * w * 4];
                for (int x = 0; x < w; ++x, p += stride, o += 4)
                {
                    const float a = components >= 4 ? read(p, 3) : 1.0f;
                    // Hydra's colour is premultiplied; frames carry straight alpha.
                    const float unpremultiply = (a > 0.0f && a < 1.0f ? 1.0f / a : 1.0f) * exposure;
                    for (int c = 0; c < 3; ++c)
                        o[c] = read(p, std::min(c, components - 1)) * unpremultiply;
                    o[3] = std::clamp(a, 0.0f, 1.0f);
                }
            });
        });
        color->Unmap();

        if (HdRenderBuffer* depth = mTasks->GetRenderOutput(HdAovTokens->depth))
        {
            depth->Resolve();
            const HdFormat df = depth->GetFormat();
            const uint8_t* d = static_cast<const uint8_t*>(depth->Map());
            if (d && int(depth->GetWidth()) == w && int(depth->GetHeight()) == h)
            {
                // Window depth to clip space, then back through the projection to the view axis: with x and y at 0
                // only these four entries of the inverse projection matter.
                const glm::dmat4 unproject = glm::inverse(proj);
                const double zz = unproject[2][2], zw = unproject[3][2], wz = unproject[2][3], ww = unproject[3][3];
                const size_t ds = HdDataSizeOfFormat(df);
                f.depth.resize(size_t(w) * h);
                withReader(df, [&](auto read) {
                    forRows(h, [&](int y) {
                        const uint8_t* p = d + size_t(h - 1 - y) * w * ds;
                        const size_t row = size_t(y) * w;
                        for (int x = 0; x < w; ++x, p += ds)
                        {
                            const float z = read(p, 0);
                            if (z >= 1.0f || f.rgba32f[(row + x) * 4 + 3] <= 0.0f)
                                continue;
                            const double ndc = 2.0 * z - 1.0;
                            f.depth[row + x] = float(-(zz * ndc + zw) / (wz * ndc + ww));
                        }
                    });
                });
            }
            depth->Unmap();
        }
        return f;
    }

    bool mOk = false;
    bool mStorm = false;
    HdPluginRenderDelegateUniqueHandle mDelegate;
    HdDriver mDriver;
    std::unique_ptr<HdRenderIndex> mIndex;
    std::unique_ptr<HdxTaskController> mTasks;
    HdEngine mEngine;
    TfRefPtr<LilaSceneIndex> mLila;
    MaterialLibrary mMaterials;
    UsdImagingSceneIndexRefPtr mMaterialIndex;
    std::map<NodeId, StageChain> mStages;
    GlfSimpleLightingContextRefPtr mEyeLight, mSceneLights;
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
    vr::Request mLast;
};
} // namespace

USD_BACKEND_EXPORT void* lilaHydraOpen(const char* plugin, int panel, const char* settings, std::string* error)
{
    try
    {
        nlohmann::json saved = nlohmann::json::parse(settings ? settings : "{}", nullptr, false);
        auto s = std::make_unique<HydraSession>(plugin, panel, saved.is_object() ? saved : nlohmann::json::object(), *error);
        if (!s->ok())
            return nullptr;
        return static_cast<vr::Session*>(s.release());
    }
    catch (const std::exception& e)
    {
        *error = e.what();
        return nullptr;
    }
}
