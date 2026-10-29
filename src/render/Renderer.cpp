#include "render/Renderer.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "render/Backdrop.h"
#include "render/Compute.h"
#include "render/DrawBatches.h"
#include "render/Environment.h"
#include "render/GpuImage.h"
#include "render/HdrImage.h"
#include "render/DisplayOptions.h"
#include "render/Surface.h"
#include "render/VirtualTexture.h"
#include "render/VulkanContext.h"

#include <glm/gtc/packing.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace lila::render
{
namespace
{
constexpr int kFramesInFlight = 2;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr int kBrdfLutSize = 128;
constexpr float kXrayOpacity = 0.35f;

struct GridPush
{
    glm::mat4 invViewProj;
    glm::mat4 viewProj;
    glm::vec4 eye;
    glm::vec4 params;
    glm::vec4 color;
    glm::vec4 axisX;
    glm::vec4 axisZ;
    glm::vec4 origin;
};

struct MeshPush
{
    glm::mat4 viewProj;
    /** The device address of the frame's instance buffer (GpuInstance entries) and this draw's first entry in it. */
    uint64_t instances = 0;
    uint32_t firstInstance = 0;
    uint32_t instancePad = 0;
    /** meshWirePS: the device address of the mesh's edge mask (RenderMesh::edgeMask), the section's first triangle and the wire width in pixels. */
    uint64_t edgeMask = 0;
    uint32_t firstTriangle = 0;
    float wireWidth = 1.0f;
    glm::vec4 reserved[2]{};
    glm::vec4 eye;
    glm::vec4 base;
    glm::vec4 params;
    glm::vec4 env;
    glm::vec4 surface;
    glm::vec3 emissive{0.0f};
    uint32_t flags = 0;
    float gamma = 1.0f;
    /** Word offset of the draw's style block (stylised shading models) in the frame's table, or kNoStyle. */
    uint32_t style = 0xffffffffu;
    /** Back-face tint: display-encoded rgb and mix, 8 bits each; 0 is off. */
    uint32_t backTint = 0;
    float displayPad = 0.0f;
    /** The device address of the frame's table: bindless texture handles, then style blocks. Maps hold 16-bit handles into it. */
    uint64_t textureTable = 0;
    uint32_t maps[2] = {0xffffffffu, 0xffffffffu}; // base color | normal << 16, ORM | emissive << 16
};
static_assert(sizeof(MeshPush) == 256, "matches MeshPush in mesh.slang");

// Draw-level bits above the material flags, matching mesh.slang.
constexpr uint32_t kVertexColorShaded = 1u << 11;
constexpr uint32_t kVertexColorUnlit = 1u << 12;
constexpr uint32_t kColorChannelShift = 13;
constexpr uint32_t kOneSidedLighting = 1u << 16;

// Placeholder textures bound where a material has no map; never evicted.
constexpr uint64_t kWhiteSrgbKey = ~uint64_t(0);
constexpr uint64_t kWhiteLinearKey = ~uint64_t(0) - 1;
constexpr uint64_t kFlatNormalKey = ~uint64_t(0) - 2;
constexpr uint32_t kMaxMaterialSets = 1024;
/** Texture table entries: a bindless index, or a UDIM set's header (this bit, then its row count). */
constexpr uint32_t kUdimEntry = 0x80000000u;
constexpr uint32_t kNoSlot = 0xffffffffu;
constexpr uint32_t kNoHandle = 0xffffu;
constexpr uint32_t kMaxBindless = 16384;
constexpr uint32_t kNoStyle = 0xffffffffu;
constexpr uint32_t kMaterialBindings = 5; // base color, normal, ORM, emissive, stroke sheet

struct ContactPush
{
    glm::mat4 viewProj;
    glm::vec4 center;
    glm::vec4 axisA;
    glm::vec4 axisB;
};

struct AoPush
{
    glm::mat4 invProj;
    glm::mat4 view;
    glm::vec4 params; // radius, intensity, pixels per world unit at depth 1, orthographic
    glm::vec4 size;   // extent, 1 / extent
    glm::vec4 options{0.0f}; // x: 1 to take normals from the depth instead of the prepass
};

constexpr VkFormat kAoNormalFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kAoFormat = VK_FORMAT_R32_SFLOAT;
constexpr uint32_t kMaxAoSets = 64;

struct PostPush
{
    glm::vec4 size;
    glm::vec4 params;  // Kuwahara on, radius, outlines on, outline width
    glm::vec4 outline; // colour, depth sensitivity
};

struct WirePush
{
    glm::mat4 viewProj;
    glm::vec4 depthPull{1.0f, 0.0f, 0.0f, 0.0f};
    uint64_t instances = 0; // as in MeshPush; each entry's color is its wire colour
    uint32_t firstInstance = 0;
    uint32_t instancePad = 0;
};
static_assert(sizeof(WirePush) == 96, "matches WirePush in wire.slang");

struct LinesPush
{
    glm::mat4 viewProj;
    glm::vec4 depthPull{1.0f, 0.0f, 0.0f, 0.0f};
};

struct PointsPush
{
    glm::mat4 viewProj;
    glm::vec4 depthPull{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 colors[3]{};
    glm::vec4 sizes{4.0f, 6.0f, 0.0f, 0.0f}; // device pixels: normal, selected
};
static_assert(sizeof(PointsPush) == 144, "matches PointsPush in points.slang");

/** Pixels' worth of world distance that lines over meshes are pulled toward the camera, plus half their width. */
constexpr double kLinePullPixels = 2.0;

/**
 * Clip-space z scale (x) and w factor (y) that move a point toward the camera along its own view ray by `pixels`
 * pixels' worth of world distance at its depth, leaving its screen position alone. Rasterizer depth bias never reaches
 * line primitives, and an edge in a crease loses the depth test to the faces either side of it without this.
 */
glm::vec4 depthPull(const glm::dmat4& proj, uint32_t heightPx, double pixels)
{
    // World size of one pixel: at unit distance for perspective, everywhere for orthographic.
    const double pixel = 2.0 / (std::abs(proj[1][1]) * double(std::max(heightPx, 1u)));
    const double a = proj[2][2];
    if (proj[3][3] != 0.0)
        return {1.0f, float(a * pixels * pixel), 0.0f, 0.0f};
    const double scale = 1.0 / (1.0 - std::min(pixels * pixel, 0.1));
    return {float(scale), float(a * (scale - 1.0)), 0.0f, 0.0f};
}

/**
 * Clips a world-space line list to the view volume, with x and y given twice the view's room. The rasterizer
 * clips only z exactly, so a wide line crossing the near plane otherwise reaches it with an end far outside its guard
 * band and floods the view. Reads v and writes only the moved ends into out, which already holds a copy (mapped memory
 * is slow to read). Lines wholly outside move to `behind`, a point behind the camera that every view clips.
 */
void clipLineList(const LineVertex* v, LineVertex* out, size_t count, const glm::dmat4& viewProj, const glm::vec3& behind)
{
    for (size_t i = 0; i + 1 < count; i += 2)
    {
        const glm::dvec3 a(v[i].pos), b(v[i + 1].pos);
        const glm::dvec4 ca = viewProj * glm::dvec4(a, 1.0), cb = viewProj * glm::dvec4(b, 1.0);
        // Inside where each is >= 0: the x and y guard planes, the far plane (reversed-Z z >= 0) and the near one (z <= w).
        auto planes = [](const glm::dvec4& c) {
            constexpr double kGuard = 2.0;
            return std::array<double, 6>{kGuard * c.w + c.x, kGuard * c.w - c.x, kGuard * c.w + c.y, kGuard * c.w - c.y, c.z, c.w - c.z};
        };
        const std::array<double, 6> da = planes(ca), db = planes(cb);
        double t0 = 0.0, t1 = 1.0;
        for (size_t k = 0; k < da.size() && t0 <= t1; ++k)
        {
            if (da[k] < 0.0 && db[k] < 0.0)
                t0 = 2.0;
            else if (da[k] < 0.0)
                t0 = std::max(t0, da[k] / (da[k] - db[k]));
            else if (db[k] < 0.0)
                t1 = std::min(t1, da[k] / (da[k] - db[k]));
        }
        if (t0 > t1)
        {
            out[i].pos = out[i + 1].pos = behind;
            continue;
        }
        if (t0 > 0.0)
            out[i].pos = glm::vec3(glm::mix(a, b, t0));
        if (t1 < 1.0)
            out[i + 1].pos = glm::vec3(glm::mix(a, b, t1));
    }
}

struct ImagePlanePush
{
    glm::mat4 viewProj;
    glm::vec4 corners[4];
    glm::vec4 uv;
    glm::vec4 params; // opacity, brightness, contrast, grayscale
    glm::vec4 texel;
};
static_assert(sizeof(ImagePlanePush) == 176, "matches ImagePlanePush in imageplane.slang");

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
};

struct GpuMesh
{
    Buffer vertices, indices, edges, edgeIndices;
    uint32_t indexCount = 0;
    uint32_t edgePointCount = 0;
    uint32_t edgeIndexCount = 0; // 0 draws edges as a plain line list
    compute::Buffer edgeMask;    // RenderMesh::edgeMask, read by meshWirePS through its address
    bool sharedVertices = false; // the vertex buffer belongs to the shareVertices mesh's entry
    std::shared_ptr<const RenderMesh> keep;
    uint64_t lastUsed = 0;
};

struct GpuPoints
{
    Buffer buffer;
    uint32_t count = 0;
    std::shared_ptr<const PointCloud> keep;
    uint64_t lastUsed = 0;
};

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct Frame
{
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkBuffer lines = VK_NULL_HANDLE;
    VmaAllocation linesAlloc = VK_NULL_HANDLE;
    void* linesMapped = nullptr;
    size_t linesCapacity = 0;
    std::vector<Buffer> staging; // texture uploads recorded into this frame, freed once its fence signals
    compute::Buffer table;       // bindless texture table, written before the frame is recorded
    compute::Buffer instances;   // GpuInstance per mesh placement, written before the frame is recorded
    vt::Feedback feedback;       // virtual texture page requests, read once the fence signals
    // GPU timestamps: frame start, before and after ambient occlusion, frame end.
    VkQueryPool timestamps = VK_NULL_HANDLE;
    bool timed = false, aoTimed = false;
};

struct Viewport
{
    uintptr_t window = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> rendered; // per swapchain image
    Image color;                       // MSAA target, resolved into the swapchain image; none at one sample
    Image depth;
    // Ambient occlusion: single-sample prepass, raw GTAO, denoised result sampled by the mesh pass.
    Image aoDepth, aoNormal, aoRaw, aoFinal;
    VkDescriptorSet gtaoSet = VK_NULL_HANDLE, denoiseSet = VK_NULL_HANDLE, aoSampleSet = VK_NULL_HANDLE;
    // Viewport post: the scene resolved here, then filtered into the frame with the prepass depth and normals.
    Image postColor;
    VkDescriptorSet postSet = VK_NULL_HANDLE;
    std::array<Frame, kFramesInFlight> frames;
    int frameIndex = 0;

    int width = 0, height = 0;
    std::atomic<bool> needsRecreate = true;
    bool pending = false;
    bool hasState = false; // nothing is drawn before the first submit
    int retries = 0;       // redraws in a row after an out-of-date swapchain
    int msaaGeneration = -1; // Impl::msaaGeneration the targets were made for
    FrameState state;
    Renderer::FrameStats timing; // render thread only; copied to stats under the mutex
    Renderer::FrameStats stats;

    // Offscreen captures draw into an owned image instead of a swapchain and read it back into a host buffer.
    bool offscreen = false;
    Image offscreenTarget;
    Buffer readback;
    void* readbackMapped = nullptr;
};

struct OffscreenJob
{
    FrameState state;
    int width = 0, height = 0;
    bool done = false, ok = false;
    std::vector<uint8_t> pixels;
    std::string error;
    Renderer::FrameStats timing;
    int repeat = 1;
};

std::vector<uint32_t> loadSpirv(const char* name)
{
    std::string err;
    std::vector<uint32_t> code = compute::loadSpirv(name, &err);
    if (code.empty())
        log::error("render: {}", err);
    return code;
}

glm::vec3 srgbToLinear(glm::vec3 c)
{
    for (int i = 0; i < 3; ++i)
        c[i] = c[i] <= 0.04045f ? c[i] / 12.92f : std::pow((c[i] + 0.055f) / 1.055f, 2.4f);
    return c;
}
} // namespace

struct Renderer::Impl
{
    VulkanContext vk;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool quit = false;
    bool started = false;

    std::map<ViewportId, std::unique_ptr<Viewport>> viewports; // guarded by mutex for add/remove/state
    std::vector<ViewportId> toRemove;
    std::condition_variable removed;
    std::deque<std::shared_ptr<OffscreenJob>> offscreenJobs; // guarded by mutex
    std::condition_variable offscreenDone;
    struct QueuedTask
    {
        GpuTask step;
        std::promise<void> done;
    };
    std::deque<std::unique_ptr<QueuedTask>> tasks; // guarded by mutex
    std::vector<std::function<void(VulkanContext&)>> stopHooks;
    ViewportId nextId = 1;

    VkPipelineLayout gridLayout = VK_NULL_HANDLE, linesLayout = VK_NULL_HANDLE, overlayTrisLayout = VK_NULL_HANDLE;
    VkPipeline gridPipeline = VK_NULL_HANDLE, linesPipeline = VK_NULL_HANDLE, overlayPipeline = VK_NULL_HANDLE;
    VkPipeline overlayTrisPipeline = VK_NULL_HANDLE;
    VkPipelineLayout meshLayout = VK_NULL_HANDLE, wireLayout = VK_NULL_HANDLE;
    VkPipeline meshPipeline = VK_NULL_HANDLE, wirePipeline = VK_NULL_HANDLE, wireXrayPipeline = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, GpuMesh> meshCache; // render thread only
    std::unordered_map<uint64_t, GpuPoints> pointCache; // render thread only
    VkPipelineLayout pointsLayout = VK_NULL_HANDLE;
    VkPipeline pointsPipeline = VK_NULL_HANDLE;
    uint64_t frameCounter = 0;
    VkDescriptorSetLayout atlasSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet atlasSet = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE;
    Image atlasImage;
    const OverlayAtlas* uploadedAtlas = nullptr;
    std::shared_ptr<const OverlayAtlas> atlasKeepAlive;
    VkFormat pipelineFormat = VK_FORMAT_UNDEFINED;
    int msaaGeneration = 0; // bumped whenever vk.msaa changes; viewports rebuild their targets to match
    VkPipeline backgroundPipeline = VK_NULL_HANDLE, linesOnTopPipeline = VK_NULL_HANDLE, tris3dPipeline = VK_NULL_HANDLE;
    /** Lines at a set width over every surface (debug drawing's on-top lines). */
    VkPipeline linesOverPipeline = VK_NULL_HANDLE;
    VkPipelineLayout contactLayout = VK_NULL_HANDLE;
    VkPipeline contactPipeline = VK_NULL_HANDLE;
    VkPipelineLayout postLayout = VK_NULL_HANDLE;
    VkPipeline postPipeline = VK_NULL_HANDLE;

    // Image-based lighting. The set always holds valid images (a black placeholder until the first environment).
    VkDescriptorSetLayout envSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet envSet = VK_NULL_HANDLE;
    VkSampler envSampler = VK_NULL_HANDLE;
    Image envCube, brdfLut;
    Buffer envUniforms;
    float envMaxLod = 0.0f;
    bool envLoaded = false;
    // Worker that builds environments; guarded by mutex.
    std::thread envWorker;
    bool envBusy = false;
    std::optional<std::string> envRequested;
    std::unique_ptr<EnvironmentData> envReady;
    // The last built environment's CPU probe, read by the main thread for overlay contrast.
    mutable std::mutex probeMutex;
    std::string probePath;
    std::shared_ptr<const EnvProbe> probe;

    // Material textures, uploaded through the frame's own command buffer with mips blitted on the GPU. Render thread only.
    struct GpuTexture
    {
        Image image; // owned; empty for a GPU image
        VkImageView view = VK_NULL_HANDLE;
        TexturePtr keep; // a GPU image's texture, kept alive while descriptors point at it
        uint32_t slot = kNoSlot; // index in the bindless array
        uint64_t lastUsed = 0;
    };
    struct MaterialSet
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t lastUsed = 0;
    };
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::map<std::array<uint64_t, kMaterialBindings>, MaterialSet> materialSets; // base color, normal, ORM, emissive, stroke keys
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool materialPool = VK_NULL_HANDLE;
    VkSampler materialSampler = VK_NULL_HANDLE;
    std::unordered_map<VkFormat, bool> blittable;
    bool warnedPool = false;
    // Bindless materials: every texture holds a slot of one runtime array (set 1 of the mesh layout), and each frame
    // writes a table the shader reads through push constants, mapping a material's maps (and UDIM tiles) to slots.
    bool bindless = false;
    VkDescriptorSetLayout bindlessLayout = VK_NULL_HANDLE;
    VkDescriptorPool bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet bindlessSet = VK_NULL_HANDLE;
    uint32_t bindlessCapacity = 0, nextSlot = 0;
    std::vector<uint32_t> freeSlots;
    bool warnedSlots = false, warnedTable = false;
    // The frame being recorded: its table, the handle of each (texture, placeholder) pair in it, and its address.
    std::vector<uint32_t> frameTable;
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> frameHandles;
    /** Style block offsets in the frame's table per (draw, material slot), for slots with a stylised shading model. */
    std::map<std::pair<const MeshDraw*, uint32_t>, uint32_t> frameStyles;
    VkDeviceAddress frameTableAddress = 0;
    /** The frame's mesh draws grouped for instancing, and the address of their instance buffer (0 if it failed). */
    DrawBatches frameBatches;
    VkDeviceAddress frameInstanceAddress = 0;
    /** Mesh draw calls and placements recorded this frame, for FrameStats. */
    uint64_t frameMeshDraws = 0, frameMeshInstances = 0;
    /** The frame draws a virtual texture map: it writes feedback. */
    bool frameUsesVt = false;
    VkPipeline meshBlendPipeline = VK_NULL_HANDLE;
    /** meshPS with the wire drawn over it (meshWirePS); null without fragment shader barycentrics, and wires draw as lines. */
    VkPipeline meshWirePipeline = VK_NULL_HANDLE, meshWireBlendPipeline = VK_NULL_HANDLE;
    // Image planes sample through set 0 of their layout, a material set holding the image as its base color.
    VkPipelineLayout imagePlaneLayout = VK_NULL_HANDLE;
    VkPipeline imageOverPipeline = VK_NULL_HANDLE, imageDepthPipeline = VK_NULL_HANDLE, imageBlendPipeline = VK_NULL_HANDLE;

    // Ambient occlusion. Set 2 of the mesh layout holds the AO texture; whiteAoSet stands in while AO is off.
    VkDescriptorSetLayout aoSampleLayout = VK_NULL_HANDLE, aoComputeLayout = VK_NULL_HANDLE;
    VkDescriptorPool aoPool = VK_NULL_HANDLE;
    VkSampler pointSampler = VK_NULL_HANDLE;
    Image whiteAo;
    VkDescriptorSet whiteAoSet = VK_NULL_HANDLE;
    VkPipelineLayout aoLayout = VK_NULL_HANDLE;
    VkPipeline gtaoPipeline = VK_NULL_HANDLE, denoisePipeline = VK_NULL_HANDLE, prepassPipeline = VK_NULL_HANDLE;
    VkPipelineLayout aoApplyLayout = VK_NULL_HANDLE;
    VkPipeline aoApplyPipeline = VK_NULL_HANDLE; // fast AO: multiplies the frame by aoFinal

    // ---------------------------------------------------------------- resources

    Image createImage(VkFormat format, VkImageUsageFlags usage, VkExtent2D extent, VkSampleCountFlagBits samples, VkImageAspectFlags aspect)
    {
        Image img;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {extent.width, extent.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = samples;
        ici.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        vmaCreateImage(vk.allocator, &ici, &aci, &img.image, &img.alloc, nullptr);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCreateImageView(vk.device, &vci, nullptr, &img.view);
        return img;
    }

    void destroyImage(Image& img)
    {
        if (img.view)
            vkDestroyImageView(vk.device, img.view, nullptr);
        if (img.image)
            vmaDestroyImage(vk.allocator, img.image, img.alloc);
        img = {};
    }

    void destroySwapchain(Viewport& v)
    {
        if (v.offscreen)
        {
            v.views.clear();
            destroyImage(v.offscreenTarget);
            if (v.readback.buffer)
                vmaDestroyBuffer(vk.allocator, v.readback.buffer, v.readback.alloc);
            v.readback = {};
            v.readbackMapped = nullptr;
        }
        for (VkImageView view : v.views)
            vkDestroyImageView(vk.device, view, nullptr);
        for (VkSemaphore s : v.rendered)
            vkDestroySemaphore(vk.device, s, nullptr);
        v.views.clear();
        v.rendered.clear();
        v.images.clear();
        destroyImage(v.color);
        destroyImage(v.depth);
        for (Image* img : {&v.aoDepth, &v.aoNormal, &v.aoRaw, &v.aoFinal, &v.postColor})
            destroyImage(*img);
    }

    bool createOffscreenTargets(Viewport& v)
    {
        LILA_ZONE();

        vkDeviceWaitIdle(vk.device);
        destroySwapchain(v);
        VkExtent2D extent{uint32_t(v.width), uint32_t(v.height)};
        v.format = pipelineFormat != VK_FORMAT_UNDEFINED ? pipelineFormat : VK_FORMAT_B8G8R8A8_SRGB;
        v.extent = extent;
        v.offscreenTarget = createImage(v.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, extent,
                                        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!v.offscreenTarget.image || !v.offscreenTarget.view)
            return false;
        v.images = {v.offscreenTarget.image};
        v.views = {v.offscreenTarget.view};

        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = VkDeviceSize(extent.width) * extent.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(vk.allocator, &bci, &aci, &v.readback.buffer, &v.readback.alloc, &info) != VK_SUCCESS)
            return false;
        v.readbackMapped = info.pMappedData;

        if (vk.msaa != VK_SAMPLE_COUNT_1_BIT)
            v.color = createImage(v.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, extent, vk.msaa,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
        v.msaaGeneration = msaaGeneration;
        v.depth = createImage(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, extent,
                              vk.msaa, VK_IMAGE_ASPECT_DEPTH_BIT);
        createAoImages(v, extent);
        if (pipelineFormat != v.format)
            createPipelines(v.format);
        return true;
    }

    bool createSwapchain(Viewport& v)
    {
        LILA_ZONE();

        if (v.offscreen)
            return createOffscreenTargets(v);
        vkDeviceWaitIdle(vk.device);
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, v.surface, &caps);
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX)
            extent = {uint32_t(v.width), uint32_t(v.height)};
        if (extent.width == 0 || extent.height == 0)
            return false;

        uint32_t fn = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, v.surface, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fn);
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, v.surface, &fn, formats.data());
        VkSurfaceFormatKHR chosen = formats[0];
        for (auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                chosen = f;

        VkSwapchainKHR old = v.swapchain;
        destroySwapchain(v);

        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = v.surface;
        sci.minImageCount = std::max(caps.minImageCount, 3u);
        if (caps.maxImageCount)
            sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
        sci.imageFormat = chosen.format;
        sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = presentMode(v.surface);
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = old;
        if (VkResult r = vkCreateSwapchainKHR(vk.device, &sci, nullptr, &v.swapchain); r != VK_SUCCESS)
        {
            log::error("render: swapchain creation failed ({})", vkResultName(r));
            v.swapchain = VK_NULL_HANDLE;
            return false;
        }
        if (old)
            vkDestroySwapchainKHR(vk.device, old, nullptr);

        v.format = chosen.format;
        v.extent = extent;
        uint32_t n = 0;
        vkGetSwapchainImagesKHR(vk.device, v.swapchain, &n, nullptr);
        v.images.resize(n);
        vkGetSwapchainImagesKHR(vk.device, v.swapchain, &n, v.images.data());
        for (VkImage img : v.images)
        {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = img;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = v.format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view;
            vkCreateImageView(vk.device, &vci, nullptr, &view);
            v.views.push_back(view);

            VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore s;
            vkCreateSemaphore(vk.device, &sem, nullptr, &s);
            v.rendered.push_back(s);
        }

        if (vk.msaa != VK_SAMPLE_COUNT_1_BIT)
            v.color = createImage(v.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, extent, vk.msaa,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
        v.msaaGeneration = msaaGeneration;
        v.depth = createImage(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, extent,
                              vk.msaa, VK_IMAGE_ASPECT_DEPTH_BIT);
        createAoImages(v, extent);

        if (pipelineFormat != v.format)
            createPipelines(v.format);
        return true;
    }

    void initFrames(Viewport& v)
    {
        for (Frame& f : v.frames)
        {
            f.feedback.destroy(vk);
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = vk.queueFamily;
            vkCreateCommandPool(vk.device, &pci, nullptr, &f.pool);
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = f.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            vkAllocateCommandBuffers(vk.device, &ai, &f.cmd);
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(vk.device, &fci, nullptr, &f.fence);
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(vk.device, &sci, nullptr, &f.acquired);
            if (vk.properties.limits.timestampComputeAndGraphics)
            {
                VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qci.queryCount = 4;
                vkCreateQueryPool(vk.device, &qci, nullptr, &f.timestamps);
            }
        }
        v.gtaoSet = allocateSet(aoPool, aoComputeLayout);
        v.denoiseSet = allocateSet(aoPool, aoComputeLayout);
        v.aoSampleSet = allocateSet(aoPool, aoSampleLayout);
        v.postSet = allocateSet(aoPool, aoSampleLayout);
    }

    void destroyViewport(Viewport& v)
    {
        vkDeviceWaitIdle(vk.device);
        for (Frame& f : v.frames)
        {
            freeStaging(f);
            compute::destroy(vk, f.table);
            compute::destroy(vk, f.instances);
            if (f.lines)
            {
                vmaUnmapMemory(vk.allocator, f.linesAlloc);
                vmaDestroyBuffer(vk.allocator, f.lines, f.linesAlloc);
            }
            vkDestroySemaphore(vk.device, f.acquired, nullptr);
            if (f.timestamps)
                vkDestroyQueryPool(vk.device, f.timestamps, nullptr);
            vkDestroyFence(vk.device, f.fence, nullptr);
            vkDestroyCommandPool(vk.device, f.pool, nullptr);
        }
        destroySwapchain(v);
        for (VkDescriptorSet set : {v.gtaoSet, v.denoiseSet, v.aoSampleSet, v.postSet})
            if (set)
                vkFreeDescriptorSets(vk.device, aoPool, 1, &set);
        if (v.swapchain)
            vkDestroySwapchainKHR(vk.device, v.swapchain, nullptr);
        if (v.surface)
            vkDestroySurfaceKHR(vk.instance, v.surface, nullptr);
    }

    void ensureLines(Frame& f, size_t bytes)
    {
        if (bytes <= f.linesCapacity)
            return;
        if (f.lines)
        {
            vmaUnmapMemory(vk.allocator, f.linesAlloc);
            vmaDestroyBuffer(vk.allocator, f.lines, f.linesAlloc);
        }
        size_t cap = std::max<size_t>(bytes * 2, 64 * 1024);
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = cap;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        vmaCreateBuffer(vk.allocator, &bci, &aci, &f.lines, &f.linesAlloc, nullptr);
        vmaMapMemory(vk.allocator, f.linesAlloc, &f.linesMapped);
        f.linesCapacity = cap;
    }

    void initDescriptors()
    {
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 1;
        lci.pBindings = &b;
        vkCreateDescriptorSetLayout(vk.device, &lci, nullptr, &atlasSetLayout);

        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 16;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        vkCreateDescriptorPool(vk.device, &pci, nullptr, &descriptorPool);

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &atlasSetLayout;
        vkAllocateDescriptorSets(vk.device, &ai, &atlasSet);

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(vk.device, &sci, nullptr, &linearSampler);

        // A 1x1 white atlas so the set is always valid before real text arrives.
        auto blank = std::make_shared<OverlayAtlas>();
        blank->width = blank->height = 1;
        blank->pixels = {255};
        uploadAtlas(blank);
        initEnvironment();
        initMaterials();
        initAo();
    }

    void destroyDescriptors()
    {
        destroyAo();
        destroyMaterials();
        destroyEnvironment();
        destroyImage(atlasImage);
        if (linearSampler)
            vkDestroySampler(vk.device, linearSampler, nullptr);
        if (descriptorPool)
            vkDestroyDescriptorPool(vk.device, descriptorPool, nullptr);
        if (atlasSetLayout)
            vkDestroyDescriptorSetLayout(vk.device, atlasSetLayout, nullptr);
        linearSampler = VK_NULL_HANDLE;
        descriptorPool = VK_NULL_HANDLE;
        atlasSetLayout = VK_NULL_HANDLE;
    }

    /** Sampled image with a full mip chain and optional cube layers, ready to be filled by uploadImage. */
    Image createTexture(VkFormat format, VkExtent2D extent, uint32_t mips, bool cube, VkImageUsageFlags extraUsage = 0,
                        VkComponentMapping swizzle = {})
    {
        Image img;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {extent.width, extent.height, 1};
        ici.mipLevels = mips;
        ici.arrayLayers = cube ? 6 : 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extraUsage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        vmaCreateImage(vk.allocator, &ici, &aci, &img.image, &img.alloc, nullptr);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img.image;
        vci.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.components = swizzle;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, ici.arrayLayers};
        vkCreateImageView(vk.device, &vci, nullptr, &img.view);
        return img;
    }

    /**
     * Copies tightly packed texels into every mip and layer of an image and leaves it shader-readable. Waits for
     * the queue, so only for rare uploads, and only when no in-flight frame can still be reading the image.
     */
    void uploadImage(VkImage image, VkExtent2D extent, uint32_t mips, uint32_t layers, size_t texelBytes,
                     const std::vector<const void*>& mipData)
    {
        LILA_ZONE();

        std::vector<VkBufferImageCopy> regions;
        size_t total = 0;
        for (uint32_t mip = 0; mip < mips; ++mip)
        {
            uint32_t w = std::max(extent.width >> mip, 1u), h = std::max(extent.height >> mip, 1u);
            VkBufferImageCopy region{};
            region.bufferOffset = total;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layers};
            region.imageExtent = {w, h, 1};
            regions.push_back(region);
            total += size_t(w) * h * layers * texelBytes;
        }

        VkBuffer staging;
        VmaAllocation stagingAlloc;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = total;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        vmaCreateBuffer(vk.allocator, &bci, &aci, &staging, &stagingAlloc, nullptr);
        void* mapped;
        vmaMapMemory(vk.allocator, stagingAlloc, &mapped);
        for (uint32_t mip = 0; mip < mips; ++mip)
        {
            size_t end = mip + 1 < mips ? size_t(regions[mip + 1].bufferOffset) : total;
            std::memcpy(static_cast<char*>(mapped) + regions[mip].bufferOffset, mipData[mip], end - size_t(regions[mip].bufferOffset));
        }
        vmaUnmapMemory(vk.allocator, stagingAlloc);
        vmaFlushAllocation(vk.allocator, stagingAlloc, 0, VK_WHOLE_SIZE);

        VkCommandPool pool;
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = vk.queueFamily;
        vkCreateCommandPool(vk.device, &pci, nullptr, &pool);
        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(vk.device, &cai, &cmd);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT, mips, layers);
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
        barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, mips, layers);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(vk.queue);
        vkDestroyCommandPool(vk.device, pool, nullptr);
        vmaDestroyBuffer(vk.allocator, staging, stagingAlloc);
    }

    /** Rare (font changes), so it just waits for the queue. */
    void uploadAtlas(std::shared_ptr<const OverlayAtlas> atlas)
    {
        LILA_ZONE();

        vkDeviceWaitIdle(vk.device);
        destroyImage(atlasImage);
        VkExtent2D extent{uint32_t(atlas->width), uint32_t(atlas->height)};
        atlasImage = createTexture(VK_FORMAT_R8_UNORM, extent, 1, false);
        uploadImage(atlasImage.image, extent, 1, 1, 1, {atlas->pixels.data()});

        VkDescriptorImageInfo ii{linearSampler, atlasImage.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = atlasSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(vk.device, 1, &w, 0, nullptr);
        uploadedAtlas = atlas.get();
        atlasKeepAlive = std::move(atlas);
    }

    // ---------------------------------------------------------------- environment

    void initEnvironment()
    {
        LILA_ZONE();

        VkDescriptorSetLayoutBinding b[3] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 3;
        lci.pBindings = b;
        vkCreateDescriptorSetLayout(vk.device, &lci, nullptr, &envSetLayout);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &envSetLayout;
        vkAllocateDescriptorSets(vk.device, &ai, &envSet);

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        vkCreateSampler(vk.device, &sci, nullptr, &envSampler);

        std::vector<uint16_t> lut = buildBrdfLut(kBrdfLutSize);
        VkExtent2D lutExtent{uint32_t(kBrdfLutSize), uint32_t(kBrdfLutSize)};
        brdfLut = createTexture(VK_FORMAT_R16G16_SFLOAT, lutExtent, 1, false);
        uploadImage(brdfLut.image, lutExtent, 1, 1, 4, {lut.data()});

        EnvironmentData placeholder;
        placeholder.size = 1;
        placeholder.mips = {std::vector<uint16_t>(6 * 4, 0)};
        uploadEnvironment(placeholder);
        envLoaded = false;
    }

    /** Swaps in a new environment. Waits for the GPU so no in-flight frame still samples the old one. */
    void uploadEnvironment(const EnvironmentData& env)
    {
        LILA_ZONE();

        vkDeviceWaitIdle(vk.device);
        destroyImage(envCube);
        destroyBuffer(envUniforms);
        uint32_t mips = uint32_t(env.mips.size());
        VkExtent2D extent{uint32_t(env.size), uint32_t(env.size)};
        envCube = createTexture(VK_FORMAT_R16G16B16A16_SFLOAT, extent, mips, true);
        std::vector<const void*> data;
        for (const auto& mip : env.mips)
            data.push_back(mip.data());
        uploadImage(envCube.image, extent, mips, 6, 8, data);
        envUniforms = createBuffer(env.sh.data(), sizeof(env.sh), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        envMaxLod = float(mips - 1);
        envLoaded = true;

        VkDescriptorImageInfo cube{envSampler, envCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo lut{linearSampler, brdfLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo ubo{envUniforms.buffer, 0, sizeof(env.sh)};
        VkWriteDescriptorSet w[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = envSet;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        }
        w[0].pImageInfo = &cube;
        w[1].pImageInfo = &lut;
        w[2].pBufferInfo = &ubo;
        vkUpdateDescriptorSets(vk.device, 3, w, 0, nullptr);
    }

    void destroyEnvironment()
    {
        destroyImage(envCube);
        destroyImage(brdfLut);
        destroyBuffer(envUniforms);
        if (envSampler)
            vkDestroySampler(vk.device, envSampler, nullptr);
        if (envSetLayout)
            vkDestroyDescriptorSetLayout(vk.device, envSetLayout, nullptr);
        envSampler = VK_NULL_HANDLE;
        envSetLayout = VK_NULL_HANDLE;
    }

    /** Starts building the environment for path on a worker. Call with mutex held and no build running. */
    void startEnvironmentBuild(const std::string& path)
    {
        if (envWorker.joinable())
            envWorker.join();
        envBusy = true;
        envRequested = path;
        envWorker = std::thread([this, path] {
            LILA_THREAD_NAME("Environment");
            auto t0 = std::chrono::steady_clock::now();
            auto env = std::make_unique<EnvironmentData>();
            HdrImage hdr;
            std::string error;
            if (path.empty())
                *env = buildStudioEnvironment();
            else if (loadHdr(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), hdr, error))
                *env = buildEnvironment(hdr);
            else
            {
                log::error("render: cannot load environment {}: {}", path, error);
                *env = buildStudioEnvironment();
            }
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            log::info("render: environment '{}' built in {} ms", path.empty() ? "studio" : path, ms);
            {
                std::shared_ptr<const EnvProbe> built = makeEnvProbe(*env);
                std::scoped_lock probeLock(probeMutex);
                probe = std::move(built);
                probePath = path;
            }

            std::scoped_lock lock(mutex);
            envReady = std::move(env);
            envBusy = false;
            for (auto& [id, v] : viewports)
                v->pending = true;
            cv.notify_all();
        });
    }

    /** With mutex held: starts a build when the wanted environment changed and hands over a finished one. */
    std::unique_ptr<EnvironmentData> pollEnvironment(const std::string& wanted)
    {
        if (!envBusy && !envReady && wanted != envRequested)
            startEnvironmentBuild(wanted);
        return std::move(envReady);
    }

    /** Host-visible buffer; on ReBAR GPUs VMA puts it in device memory, so no staging copy is needed. */
    Buffer createBuffer(const void* data, size_t size, VkBufferUsageFlags usage)
    {
        Buffer b;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = std::max<size_t>(size, 16);
        bci.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        vmaCreateBuffer(vk.allocator, &bci, &aci, &b.buffer, &b.alloc, &info);
        constexpr size_t kBlock = size_t(4) << 20;
        if (size > kBlock)
            tbb::parallel_for(size_t(0), (size + kBlock - 1) / kBlock, [&](size_t k) {
                std::memcpy(static_cast<char*>(info.pMappedData) + k * kBlock, static_cast<const char*>(data) + k * kBlock,
                            std::min(kBlock, size - k * kBlock));
            });
        else if (size)
            std::memcpy(info.pMappedData, data, size);
        vmaFlushAllocation(vk.allocator, b.alloc, 0, VK_WHOLE_SIZE);
        return b;
    }

    /**
     * A device-local copy of data, filled by a copy recorded into cmd. The staging buffer is freed once the frame's fence
     * signals. Falls back to host memory when device memory is full.
     */
    Buffer deviceBuffer(VkCommandBuffer cmd, Frame& f, const void* data, size_t size, VkBufferUsageFlags usage)
    {
        Buffer b;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = std::max<size_t>(size, 16);
        bci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateBuffer(vk.allocator, &bci, &aci, &b.buffer, &b.alloc, nullptr) != VK_SUCCESS)
            return createBuffer(data, size, usage);
        if (size)
        {
            Buffer staging = createBuffer(data, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            f.staging.push_back(staging);
            const VkBufferCopy region{0, 0, size};
            vkCmdCopyBuffer(cmd, staging.buffer, b.buffer, 1, &region);
        }
        return b;
    }

    void destroyBuffer(Buffer& b)
    {
        if (b.buffer)
            vmaDestroyBuffer(vk.allocator, b.buffer, b.alloc);
        b = {};
    }

    /**
     * The mesh's GPU copy, made on first use. With cmd and f the copy goes to device memory through a copy recorded into
     * cmd; without them it reads from host memory, which is slow for dense meshes once the BAR heap is full.
     */
    GpuMesh* gpuMesh(const std::shared_ptr<const RenderMesh>& mesh, VkCommandBuffer cmd = VK_NULL_HANDLE, Frame* f = nullptr)
    {
        LILA_ZONE();

        auto upload = [&](const void* data, size_t size, VkBufferUsageFlags usage) {
            return f ? deviceBuffer(cmd, *f, data, size, usage) : createBuffer(data, size, usage);
        };
        // Looked up first: inserting this entry may rehash the cache under the base's.
        Buffer shared;
        if (mesh->shareVertices)
            shared = gpuMesh(mesh->shareVertices, cmd, f)->vertices;
        auto it = meshCache.find(mesh->key);
        if (it == meshCache.end())
        {
            GpuMesh g;
            if (mesh->shareVertices)
            {
                g.vertices = shared;
                g.sharedVertices = true;
            }
            else if (mesh->dynamic)
            {
                DynamicVertices& dv = *mesh->dynamic;
                std::scoped_lock lock(dv.mutex);
                g.vertices = upload(dv.vertices.data(), dv.vertices.size() * sizeof(MeshVertex),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                dv.dirty.clear();
                dv.allDirty = false;
            }
            else
                g.vertices = upload(mesh->vertices.data(), mesh->vertices.size() * sizeof(MeshVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            g.indices = upload(mesh->indices.data(), mesh->indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            g.edges = upload(mesh->edgePoints.data(), mesh->edgePoints.size() * sizeof(glm::vec3),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | (mesh->dynamic ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0));
            g.indexCount = uint32_t(mesh->indices.size());
            g.edgePointCount = uint32_t(mesh->edgePoints.size());
            if (!mesh->edgeMask.empty() && meshWirePipeline)
                g.edgeMask = maskBuffer(cmd, f, mesh->edgeMask);
            if (!mesh->edgeIndices.empty())
            {
                g.edgeIndices = upload(mesh->edgeIndices.data(), mesh->edgeIndices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
                g.edgeIndexCount = uint32_t(mesh->edgeIndices.size());
            }
            g.keep = mesh;
            it = meshCache.emplace(mesh->key, std::move(g)).first;
        }
        else if (mesh->shareVertices)
            it->second.vertices = shared;
        it->second.lastUsed = frameCounter;
        return &it->second;
    }

    /** A storage buffer with a device address holding words, copied through a staging buffer when cmd and f are given. */
    compute::Buffer maskBuffer(VkCommandBuffer cmd, Frame* f, const std::vector<uint32_t>& words)
    {
        const size_t bytes = words.size() * sizeof(uint32_t);
        try
        {
            if (!f)
                return compute::hostBuffer(vk, words.data(), bytes);
            compute::Buffer b = compute::makeBuffer(vk, bytes, 0, compute::Host::None);
            Buffer staging = createBuffer(words.data(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            f->staging.push_back(staging);
            const VkBufferCopy region{0, 0, bytes};
            vkCmdCopyBuffer(cmd, staging.buffer, b.buffer, 1, &region);
            return b;
        }
        catch (const std::exception& e)
        {
            log::warn("render: {}; the wire draws as lines", e.what());
            return {};
        }
    }

    /**
     * Drops GPU copies no frame has drawn for a while, and at once those nothing else holds any more: views draw on
     * demand, so an idle viewport would otherwise keep a replaced dense mesh in memory indefinitely.
     */
    void evictMeshes()
    {
        const bool sweep = frameCounter % 120 == 0;
        std::vector<uint64_t> stale;
        for (auto& [key, g] : meshCache)
            if ((sweep && g.lastUsed + 240 < frameCounter) || (g.keep.use_count() == 1 && g.lastUsed != frameCounter))
                stale.push_back(key);
        if (stale.empty())
            return;
        vkDeviceWaitIdle(vk.device);
        for (uint64_t key : stale)
        {
            GpuMesh& g = meshCache[key];
            if (!g.sharedVertices)
                destroyBuffer(g.vertices);
            destroyBuffer(g.indices);
            destroyBuffer(g.edges);
            destroyBuffer(g.edgeIndices);
            compute::destroy(vk, g.edgeMask);
            meshCache.erase(key);
        }
    }

    /** Drops component dots no frame has drawn for a while: each selection change makes a new cloud. */
    void evictPoints()
    {
        if (frameCounter % 30 != 0)
            return;
        std::vector<uint64_t> stale;
        for (auto& [key, g] : pointCache)
            if (g.lastUsed + 8 < frameCounter)
                stale.push_back(key);
        if (stale.empty())
            return;
        vkDeviceWaitIdle(vk.device);
        for (uint64_t key : stale)
        {
            destroyBuffer(pointCache[key].buffer);
            pointCache.erase(key);
        }
    }

    void destroyMeshes()
    {
        for (auto& [key, g] : pointCache)
            destroyBuffer(g.buffer);
        pointCache.clear();
        for (auto& [key, g] : meshCache)
        {
            if (!g.sharedVertices)
                destroyBuffer(g.vertices);
            destroyBuffer(g.indices);
            destroyBuffer(g.edges);
            destroyBuffer(g.edgeIndices);
            compute::destroy(vk, g.edgeMask);
        }
        meshCache.clear();
    }

    // ---------------------------------------------------------------- materials

    void initMaterials()
    {
        LILA_ZONE();

        VkDescriptorSetLayoutBinding b[kMaterialBindings];
        for (uint32_t i = 0; i < kMaterialBindings; ++i)
            b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = kMaterialBindings;
        lci.pBindings = b;
        vkCreateDescriptorSetLayout(vk.device, &lci, nullptr, &materialSetLayout);

        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxMaterialSets * kMaterialBindings};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = kMaxMaterialSets;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &size;
        vkCreateDescriptorPool(vk.device, &pci, nullptr, &materialPool);

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        sci.anisotropyEnable = vk.anisotropy;
        sci.maxAnisotropy = vk.anisotropy ? std::min(8.0f, vk.properties.limits.maxSamplerAnisotropy) : 1.0f;
        vkCreateSampler(vk.device, &sci, nullptr, &materialSampler);
        initBindless();

        auto placeholder = [&](uint64_t key, VkFormat format, std::array<uint8_t, 4> texel) {
            GpuTexture& t = textures[key];
            t.image = createTexture(format, {1, 1}, 1, false);
            t.view = t.image.view;
            uploadImage(t.image.image, {1, 1}, 1, 1, 4, {texel.data()});
            assignSlot(t);
        };
        placeholder(kWhiteSrgbKey, VK_FORMAT_R8G8B8A8_SRGB, {255, 255, 255, 255});
        placeholder(kWhiteLinearKey, VK_FORMAT_R8G8B8A8_UNORM, {255, 255, 255, 255});
        placeholder(kFlatNormalKey, VK_FORMAT_R8G8B8A8_UNORM, {128, 128, 255, 255});
    }

    /** Whether images of a format can have their mips blitted (and be filtered). */
    bool canBlit(VkFormat f)
    {
        auto it = blittable.find(f);
        if (it != blittable.end())
            return it->second;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(vk.physical, f, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        return blittable[f] = (fp.optimalTilingFeatures & need) == need;
    }

    /** The bindless array, when the device has descriptor indexing and the bindless mesh shader is there. */
    void initBindless()
    {
        if (!vk.descriptorIndexing || loadSpirv("mesh_bindless.spv").empty())
            return;
        LILA_ZONE();

        VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
        vkGetPhysicalDeviceProperties2(vk.physical, &p2);
        // The rest of the mesh layout (environment and AO sets) counts against the same per-stage limits.
        uint32_t limit = std::min({p12.maxDescriptorSetUpdateAfterBindSampledImages, p12.maxDescriptorSetUpdateAfterBindSamplers,
                                   p12.maxPerStageDescriptorUpdateAfterBindSampledImages, p12.maxPerStageDescriptorUpdateAfterBindSamplers,
                                   p12.maxPerStageUpdateAfterBindResources});
        bindlessCapacity = std::min(kMaxBindless, limit > 16 ? limit - 16 : 0);
        if (bindlessCapacity < 64)
            return;

        VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                         VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bfi.bindingCount = 1;
        bfi.pBindingFlags = &flags;
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, bindlessCapacity, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.pNext = &bfi;
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        lci.bindingCount = 1;
        lci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(vk.device, &lci, nullptr, &bindlessLayout) != VK_SUCCESS)
            return;

        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, bindlessCapacity};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pci.maxSets = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &size;
        if (vkCreateDescriptorPool(vk.device, &pci, nullptr, &bindlessPool) != VK_SUCCESS)
        {
            destroyBindless();
            return;
        }
        VkDescriptorSetVariableDescriptorCountAllocateInfo vci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
        vci.descriptorSetCount = 1;
        vci.pDescriptorCounts = &bindlessCapacity;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.pNext = &vci;
        ai.descriptorPool = bindlessPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &bindlessLayout;
        if (vkAllocateDescriptorSets(vk.device, &ai, &bindlessSet) != VK_SUCCESS)
        {
            destroyBindless();
            return;
        }
        bindless = true;
        log::info("render: bindless materials ({} textures)", bindlessCapacity);
    }

    void destroyBindless()
    {
        if (bindlessPool)
            vkDestroyDescriptorPool(vk.device, bindlessPool, nullptr);
        if (bindlessLayout)
            vkDestroyDescriptorSetLayout(vk.device, bindlessLayout, nullptr);
        bindlessPool = VK_NULL_HANDLE;
        bindlessLayout = VK_NULL_HANDLE;
        bindlessSet = VK_NULL_HANDLE;
        bindless = false;
        freeSlots.clear();
        nextSlot = 0;
    }

    /** Gives a texture a bindless slot pointing at its view. Without one it samples as its placeholder. */
    void assignSlot(GpuTexture& t)
    {
        if (!bindless || t.slot != kNoSlot || !t.view)
            return;
        if (!freeSlots.empty())
        {
            t.slot = freeSlots.back();
            freeSlots.pop_back();
        }
        else if (nextSlot < bindlessCapacity)
            t.slot = nextSlot++;
        else
        {
            if (!warnedSlots)
                log::warn("render: more than {} material textures; the rest draw blank", bindlessCapacity);
            warnedSlots = true;
            return;
        }
        VkDescriptorImageInfo info{materialSampler, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = bindlessSet;
        w.dstBinding = 0;
        w.dstArrayElement = t.slot;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(vk.device, 1, &w, 0, nullptr);
    }

    void destroyMaterials()
    {
        for (auto& [key, t] : textures)
            destroyImage(t.image);
        textures.clear();
        materialSets.clear();
        destroyBindless();
        if (materialSampler)
            vkDestroySampler(vk.device, materialSampler, nullptr);
        if (materialPool)
            vkDestroyDescriptorPool(vk.device, materialPool, nullptr);
        if (materialSetLayout)
            vkDestroyDescriptorSetLayout(vk.device, materialSetLayout, nullptr);
        materialSampler = VK_NULL_HANDLE;
        materialPool = VK_NULL_HANDLE;
        materialSetLayout = VK_NULL_HANDLE;
    }

    static void mipBarrier(VkCommandBuffer cmd, VkImage image, uint32_t baseMip, uint32_t mips, VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, 0, 1};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    /**
     * Records the upload of a texture and its mip chain into cmd, ahead of the frame's rendering. The staging buffer
     * lives until the frame's fence signals. False for malformed images, which then draw as if absent.
     */
    bool uploadTexture(VkCommandBuffer cmd, Frame& f, const TextureImage& t)
    {
        LILA_ZONE();

        const size_t bytes = size_t(t.width) * size_t(t.height) * size_t(texelBytes(t.format));
        if (t.width <= 0 || t.height <= 0 || t.rgba.size() < bytes)
            return false;
        VkFormat format = vkFormat(t.format, t.srgb);
        VkExtent2D extent{uint32_t(t.width), uint32_t(t.height)};
        uint32_t mips = 1;
        if (canBlit(format))
            while ((std::max(extent.width, extent.height) >> mips) > 0)
                ++mips;

        GpuTexture g;
        g.image = createTexture(format, extent, mips, false, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, sampleSwizzle(t.format));
        g.view = g.image.view;
        g.lastUsed = frameCounter;
        Buffer staging = createBuffer(t.rgba.data(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        f.staging.push_back(staging);

        constexpr auto transfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mipBarrier(cmd, g.image.image, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                   transfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, g.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        for (uint32_t mip = 1; mip < mips; ++mip)
        {
            mipBarrier(cmd, g.image.image, mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transfer,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, transfer, VK_ACCESS_2_TRANSFER_READ_BIT);
            int32_t sw = int32_t(std::max(extent.width >> (mip - 1), 1u)), sh = int32_t(std::max(extent.height >> (mip - 1), 1u));
            int32_t dw = int32_t(std::max(extent.width >> mip, 1u)), dh = int32_t(std::max(extent.height >> mip, 1u));
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
            blit.srcOffsets[1] = {sw, sh, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
            blit.dstOffsets[1] = {dw, dh, 1};
            vkCmdBlitImage(cmd, g.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);
        }
        constexpr auto sampled = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        if (mips > 1)
            mipBarrier(cmd, g.image.image, 0, mips - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, transfer,
                       VK_ACCESS_2_TRANSFER_READ_BIT, sampled, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        mipBarrier(cmd, g.image.image, mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, transfer,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, sampled, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        assignSlot(g);
        textures[t.key] = g;
        return true;
    }

    /** Records a GPU image's queued updates and registers it as a texture on first use. False when it has no device image. */
    bool prepareGpuImage(VkCommandBuffer cmd, Frame& f, const TexturePtr& t)
    {
        std::vector<compute::Buffer> staging;
        const bool ok = t->gpu->prepare(vk, cmd, staging);
        for (const compute::Buffer& b : staging)
            f.staging.push_back({b.buffer, b.alloc});
        if (!ok)
            return false;
        auto [it, added] = textures.try_emplace(t->key);
        if (added)
        {
            it->second.view = t->gpu->view();
            it->second.keep = t;
            it->second.lastUsed = frameCounter;
            assignSlot(it->second);
        }
        return true;
    }

    /**
     * Key of an uploaded texture, or the placeholder's when the map is missing or failed to upload. A UDIM set gives its
     * first uploaded tile (what the fixed sampler path shows) and keeps all its tiles alive.
     */
    uint64_t textureKey(const TexturePtr& t, uint64_t placeholder)
    {
        if (!t)
            return placeholder;
        if (t->vt)
            return bindless ? t->key : placeholder;
        if (t->isUdimSet())
        {
            uint64_t first = placeholder;
            for (const UdimTile& tile : t->tiles)
                if (tile.image && !tile.image->isUdimSet())
                    if (uint64_t k = textureKey(tile.image, placeholder); k != placeholder && first == placeholder)
                        first = k;
            return first;
        }
        auto it = textures.find(t->key);
        if (it == textures.end())
            return placeholder;
        it->second.lastUsed = frameCounter;
        return t->key;
    }

    /** Uploads a map (every tile of a UDIM set) the first time a frame uses it, and lands a GPU image's updates. */
    void prepareTexture(VkCommandBuffer cmd, Frame& f, const TexturePtr& t, const char* what)
    {
        if (!t)
            return;
        if (t->isUdimSet())
        {
            for (const UdimTile& tile : t->tiles)
                if (tile.image && !tile.image->isUdimSet())
                    prepareTexture(cmd, f, tile.image, what);
            return;
        }
        if (t->vt)
        {
            for (const TexturePtr& chunk : vt::chunkTextures(*t))
                prepareGpuImage(cmd, f, chunk);
            frameUsesVt = true;
        }
        else if (t->gpu)
            prepareGpuImage(cmd, f, t);
        else if (!textures.contains(t->key) && !uploadTexture(cmd, f, *t))
            log::warn("render: skipped a malformed {}x{} {}", t->width, t->height, what);
    }

    /** Uploads every map this frame's materials use for the first time, then writes the frame's texture table. Runs before rendering begins. */
    void prepareTextures(VkCommandBuffer cmd, Frame& f, const FrameState& s)
    {
        LILA_ZONE();

        frameUsesVt = false;
        if (bindless)
        {
            std::vector<compute::Buffer> staging;
            vt::recordFrame(vk, cmd, staging);
            for (const compute::Buffer& b : staging)
                f.staging.push_back({b.buffer, b.alloc});
        }

        for (const MeshDraw& d : s.meshes)
        {
            if (!(d.flags & DrawShaded))
                continue;
            for (const Material& mat : d.materials)
                for (const TexturePtr* t : {&mat.baseColorMap, &mat.normalMap, &mat.ormMap, &mat.emissiveMap, &mat.style.strokeMap})
                    prepareTexture(cmd, f, *t, "texture");
        }
        for (const ImagePlaneDraw& p : s.imagePlanes)
            prepareTexture(cmd, f, p.image, "image plane");
        writeTextureTable(f, s);
    }

    /** The bindless slot a texture key samples, its placeholder's when it has none. */
    uint32_t slotOf(uint64_t key, uint64_t placeholder)
    {
        auto it = textures.find(key);
        if (it != textures.end() && it->second.slot != kNoSlot)
            return it->second.slot;
        it = textures.find(placeholder);
        return it != textures.end() && it->second.slot != kNoSlot ? it->second.slot : 0;
    }

    /**
     * The frame's table handle for a map, added on first use: one entry holding the slot for a plain texture, or for a
     * UDIM set a header (kUdimEntry | rows), the placeholder's slot, then ten slots per row of tiles (U 0..9).
     */
    uint32_t tableHandle(const TexturePtr& t, uint64_t placeholder)
    {
        if (!t)
            return kNoHandle;
        auto [it, added] = frameHandles.try_emplace({t->key, placeholder}, kNoHandle);
        if (!added)
            return it->second;
        const uint32_t handle = uint32_t(frameTable.size());
        const uint32_t fallback = slotOf(placeholder, placeholder);
        if (t->vt)
            vt::tableEntry(*t, frameTable, [&](const TexturePtr& chunk) { return slotOf(chunk->key, placeholder); });
        else if (!t->isUdimSet())
            frameTable.push_back(slotOf(textureKey(t, placeholder), placeholder));
        else
        {
            int rows = 1;
            for (const UdimTile& tile : t->tiles)
                if (tile.udim >= 1001)
                    rows = std::max(rows, (tile.udim - 1001) / 10 + 1);
            rows = std::min(rows, 100);
            frameTable.push_back(kUdimEntry | uint32_t(rows));
            frameTable.push_back(fallback);
            const size_t first = frameTable.size();
            frameTable.resize(first + size_t(rows) * 10, fallback);
            for (const UdimTile& tile : t->tiles)
            {
                const int index = tile.udim - 1001;
                if (index >= 0 && index < rows * 10 && tile.image && !tile.image->isUdimSet())
                    frameTable[first + size_t(index)] = slotOf(textureKey(tile.image, placeholder), placeholder);
            }
        }
        if (handle >= kNoHandle)
        {
            if (!warnedTable)
                log::warn("render: too many material maps in one frame; some draw without textures");
            warnedTable = true;
            frameTable.resize(handle);
            return it->second = kNoHandle;
        }
        return it->second = handle;
    }

    /** Builds the table every bindless draw this frame reads its maps through and copies it into the frame's buffer. */
    void writeTextureTable(Frame& f, const FrameState& s)
    {
        frameTable.clear();
        frameHandles.clear();
        frameStyles.clear();
        // The virtual texture feedback header, filled in once the frame knows its feedback buffer.
        frameTable.assign(vt::kTableHeaderWords, 0u);
        if (bindless)
            for (const MeshDraw& d : s.meshes)
            {
                if (!(d.flags & DrawShaded))
                    continue;
                for (const Material& mat : d.materials)
                {
                    tableHandle(mat.baseColorMap, kWhiteSrgbKey);
                    tableHandle(mat.normalMap, kFlatNormalKey);
                    tableHandle(mat.ormMap, kWhiteLinearKey);
                    tableHandle(mat.emissiveMap, kWhiteSrgbKey);
                    if (mat.style.strokeMap && !mat.style.strokeMap->isUdimSet())
                        tableHandle(mat.style.strokeMap, kWhiteLinearKey);
                }
            }
        writeStyles(s);
        if (frameTable.size() <= vt::kTableHeaderWords)
            frameTable.push_back(slotOf(kWhiteSrgbKey, kWhiteSrgbKey));
        const size_t bytes = frameTable.size() * sizeof(uint32_t);
        if (f.table.size < bytes)
        {
            // The frame's fence has signalled, so nothing reads the old table any more.
            compute::destroy(vk, f.table);
            try
            {
                f.table = compute::makeBuffer(vk, std::max<size_t>(bytes * 2, 4096), 0, compute::Host::Write);
            }
            catch (const std::exception& e)
            {
                log::error("render: {}", e.what());
                frameTableAddress = 0;
                return;
            }
        }
        std::memcpy(f.table.mapped, frameTable.data(), bytes);
        vmaFlushAllocation(vk.allocator, f.table.alloc, 0, VK_WHOLE_SIZE);
        frameTableAddress = f.table.address;
    }

    /** Groups the frame's mesh draws for instancing and writes their placements into the frame's instance buffer. */
    void writeInstances(Frame& f, const FrameState& s)
    {
        LILA_ZONE();

        frameBatches = batchDraws(s, meshWirePipeline != VK_NULL_HANDLE);
        frameInstanceAddress = 0;
        const size_t bytes = std::max<size_t>(frameBatches.instances.size(), 1) * sizeof(GpuInstance);
        if (f.instances.size < bytes)
        {
            // The frame's fence has signalled, so nothing reads the old buffer any more.
            compute::destroy(vk, f.instances);
            try
            {
                f.instances = compute::makeBuffer(vk, std::max<size_t>(bytes * 2, 64 * sizeof(GpuInstance)), 0, compute::Host::Write);
            }
            catch (const std::exception& e)
            {
                log::error("render: {}", e.what());
                frameBatches = {};
                return;
            }
        }
        if (!frameBatches.instances.empty())
        {
            std::memcpy(f.instances.mapped, frameBatches.instances.data(), frameBatches.instances.size() * sizeof(GpuInstance));
            vmaFlushAllocation(vk.allocator, f.instances.alloc, 0, VK_WHOLE_SIZE);
        }
        frameInstanceAddress = f.instances.address;
    }

    /** Appends a style block per shaded draw section whose material uses a stylised shading model. */
    void writeStyles(const FrameState& s)
    {
        for (const MeshDraw& d : s.meshes)
        {
            if (!(d.flags & DrawShaded) || !d.mesh)
                continue;
            for (uint32_t slot = 0; slot < d.materials.size(); ++slot)
            {
                const MaterialStyle& style = d.materials[slot].style;
                if (style.model == ShadingModel::DefaultLit)
                    continue;
                uint32_t stroke = 0xffffffffu;
                if (style.strokeMap && !style.strokeMap->isUdimSet() && textureKey(style.strokeMap, 0) != 0)
                {
                    // The fixed-sampler path binds the sheet itself; bindless reads its slot through the table.
                    auto it = frameHandles.find({style.strokeMap->key, kWhiteLinearKey});
                    if (!bindless)
                        stroke = 0;
                    else if (it != frameHandles.end() && it->second != kNoHandle)
                        stroke = it->second;
                }
                const auto block = packStyle(style, stroke, d.mesh->boundsMin, d.mesh->boundsMax, s.zUp);
                frameStyles[{&d, slot}] = uint32_t(frameTable.size());
                frameTable.insert(frameTable.end(), block.begin(), block.end());
            }
        }
    }

    /** Uploads the meshes this frame draws for the first time into device memory. Runs before rendering begins. */
    void prepareMeshes(VkCommandBuffer cmd, Frame& f, const FrameState& s)
    {
        LILA_ZONE();

        const size_t staged = f.staging.size();
        for (const MeshDraw& d : s.meshes)
            if (d.mesh && !meshCache.contains(d.mesh->key))
                gpuMesh(d.mesh, cmd, &f);
        for (const PointDraw& p : s.points)
            if (p.cloud && !p.cloud->points.empty() && !pointCache.contains(p.cloud->key))
            {
                GpuPoints g;
                g.buffer = deviceBuffer(cmd, f, p.cloud->points.data(), p.cloud->points.size() * sizeof(PointVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                g.count = uint32_t(p.cloud->points.size());
                g.keep = p.cloud;
                pointCache.emplace(p.cloud->key, std::move(g));
            }
        if (f.staging.size() != staged)
            memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    /** Copies the vertices edited since the last frame into the GPU copies of dynamic meshes. Runs before rendering begins. */
    void prepareDynamicMeshes(VkCommandBuffer cmd, Frame& f, const FrameState& s)
    {
        LILA_ZONE();

        std::vector<VkBufferCopy> regions;
        std::vector<MeshVertex> data;
        bool first = true;
        for (const MeshDraw& d : s.meshes)
        {
            if (!d.mesh || !d.mesh->dynamic)
                continue;
            auto it = meshCache.find(d.mesh->key);
            if (it == meshCache.end())
            {
                gpuMesh(d.mesh);
                continue;
            }
            DynamicVertices& dv = *d.mesh->dynamic;
            regions.clear();
            data.clear();
            {
                std::scoped_lock lock(dv.mutex);
                if (dv.allDirty)
                {
                    data = dv.vertices;
                    regions.push_back({0, 0, data.size() * sizeof(MeshVertex)});
                }
                else if (!dv.dirty.empty())
                {
                    std::sort(dv.dirty.begin(), dv.dirty.end());
                    dv.dirty.erase(std::unique(dv.dirty.begin(), dv.dirty.end()), dv.dirty.end());
                    // Short gaps are cheaper to copy than to split into separate regions.
                    for (size_t i = 0; i < dv.dirty.size();)
                    {
                        size_t j = i + 1;
                        while (j < dv.dirty.size() && dv.dirty[j] - dv.dirty[j - 1] <= 16)
                            ++j;
                        uint32_t begin = dv.dirty[i], end = std::min<uint32_t>(dv.dirty[j - 1] + 1, uint32_t(dv.vertices.size()));
                        if (begin < end)
                        {
                            regions.push_back({data.size() * sizeof(MeshVertex), begin * sizeof(MeshVertex), (end - begin) * sizeof(MeshVertex)});
                            data.insert(data.end(), dv.vertices.begin() + begin, dv.vertices.begin() + end);
                        }
                        i = j;
                    }
                }
                dv.dirty.clear();
                dv.allDirty = false;
            }
            // Every snapshot of a live mesh shares one key, so the wire's points come from whichever copy was uploaded first.
            GpuMesh& g = it->second;
            const bool edgesStale = g.keep != d.mesh && !d.mesh->edgePoints.empty() && d.mesh->edgePoints.size() == g.edgePointCount;
            if (g.keep != d.mesh)
                g.keep = d.mesh;
            if (regions.empty() && !edgesStale)
                continue;
            if (first)
            {
                // Earlier frames may still read these vertices.
                memoryBarrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                first = false;
            }
            if (!regions.empty())
            {
                Buffer staging = createBuffer(data.data(), data.size() * sizeof(MeshVertex), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
                f.staging.push_back(staging);
                vkCmdCopyBuffer(cmd, staging.buffer, g.vertices.buffer, uint32_t(regions.size()), regions.data());
            }
            if (edgesStale)
            {
                const size_t bytes = d.mesh->edgePoints.size() * sizeof(glm::vec3);
                Buffer staging = createBuffer(d.mesh->edgePoints.data(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
                f.staging.push_back(staging);
                const VkBufferCopy region{0, 0, bytes};
                vkCmdCopyBuffer(cmd, staging.buffer, g.edges.buffer, 1, &region);
            }
        }
        if (!first)
            memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                          VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
    }

    static void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                              VkAccessFlags2 dstAccess)
    {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    /** Descriptor set binding a material's maps, made on first use and kept while frames use it. */
    VkDescriptorSet materialSet(const Material* m)
    {
        std::array<uint64_t, kMaterialBindings> keys = {kWhiteSrgbKey, kFlatNormalKey, kWhiteLinearKey, kWhiteSrgbKey, kWhiteLinearKey};
        if (m)
            keys = {textureKey(m->baseColorMap, kWhiteSrgbKey), textureKey(m->normalMap, kFlatNormalKey), textureKey(m->ormMap, kWhiteLinearKey),
                    textureKey(m->emissiveMap, kWhiteSrgbKey), textureKey(m->style.strokeMap, kWhiteLinearKey)};
        auto it = materialSets.find(keys);
        if (it == materialSets.end())
        {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &materialSetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(vk.device, &ai, &set) != VK_SUCCESS)
            {
                if (!warnedPool)
                    log::warn("render: out of material descriptor sets; some materials draw without textures");
                warnedPool = true;
                std::array<uint64_t, kMaterialBindings> fallback = {kWhiteSrgbKey, kFlatNormalKey, kWhiteLinearKey, kWhiteSrgbKey, kWhiteLinearKey};
                auto fb = materialSets.find(fallback);
                return fb != materialSets.end() ? fb->second.set : VK_NULL_HANDLE;
            }
            VkDescriptorImageInfo info[kMaterialBindings];
            VkWriteDescriptorSet w[kMaterialBindings]{};
            for (uint32_t i = 0; i < kMaterialBindings; ++i)
            {
                info[i] = {materialSampler, textures[keys[i]].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = set;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = &info[i];
            }
            vkUpdateDescriptorSets(vk.device, kMaterialBindings, w, 0, nullptr);
            it = materialSets.emplace(keys, MaterialSet{set, 0}).first;
        }
        it->second.lastUsed = frameCounter;
        return it->second.set;
    }

    /** Drops textures and sets no frame has used for a while. */
    void evictTextures()
    {
        if (frameCounter % 120 != 60)
            return;
        auto stale = [&](uint64_t last) { return last + 240 < frameCounter; };
        std::vector<uint64_t> deadTextures;
        for (auto& [key, t] : textures)
            if (key < kFlatNormalKey && stale(t.lastUsed))
                deadTextures.push_back(key);
        std::vector<std::array<uint64_t, kMaterialBindings>> deadSets;
        for (auto& [keys, set] : materialSets)
            if (stale(set.lastUsed) || std::ranges::any_of(keys, [&](uint64_t k) { return std::ranges::find(deadTextures, k) != deadTextures.end(); }))
                deadSets.push_back(keys);
        if (deadTextures.empty() && deadSets.empty())
            return;
        vkDeviceWaitIdle(vk.device);
        for (const auto& keys : deadSets)
        {
            vkFreeDescriptorSets(vk.device, materialPool, 1, &materialSets[keys].set);
            materialSets.erase(keys);
        }
        for (uint64_t key : deadTextures)
        {
            GpuTexture& t = textures[key];
            destroyImage(t.image);
            if (t.slot != kNoSlot)
                freeSlots.push_back(t.slot);
            textures.erase(key);
        }
    }

    void freeStaging(Frame& f)
    {
        for (Buffer& b : f.staging)
            destroyBuffer(b);
        f.staging.clear();
    }

    /** Frees the staging of frames the GPU has finished; true while some frame still holds staging. */
    bool reclaimStaging()
    {
        bool held = false;
        for (auto& [id, v] : viewports)
            for (Frame& f : v->frames)
                if (!f.staging.empty())
                {
                    if (vkGetFenceStatus(vk.device, f.fence) == VK_SUCCESS)
                        freeStaging(f);
                    else
                        held = true;
                }
        return held;
    }

    // ---------------------------------------------------------------- ambient occlusion

    VkDescriptorSet allocateSet(VkDescriptorPool pool, VkDescriptorSetLayout layout)
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(vk.device, &ai, &set) != VK_SUCCESS)
            log::error("render: out of ambient occlusion descriptor sets");
        return set;
    }

    void writeImage(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkImageView view, VkImageLayout layout)
    {
        if (!set)
            return;
        VkDescriptorImageInfo ii{type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? VK_NULL_HANDLE : pointSampler, view, layout};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = type;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(vk.device, 1, &w, 0, nullptr);
    }

    void initAo()
    {
        LILA_ZONE();

        VkDescriptorSetLayoutBinding sb[3];
        for (uint32_t i = 0; i < 3; ++i)
            sb[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 3;
        lci.pBindings = sb;
        vkCreateDescriptorSetLayout(vk.device, &lci, nullptr, &aoSampleLayout);

        VkDescriptorSetLayoutBinding cb[4];
        for (uint32_t i = 0; i < 4; ++i)
            cb[i] = {i, i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        lci.bindingCount = 4;
        lci.pBindings = cb;
        vkCreateDescriptorSetLayout(vk.device, &lci, nullptr, &aoComputeLayout);

        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxAoSets * 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxAoSets}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = kMaxAoSets;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        vkCreateDescriptorPool(vk.device, &pci, nullptr, &aoPool);

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(vk.device, &sci, nullptr, &pointSampler);

        float one = 1.0f;
        whiteAo = createTexture(kAoFormat, {1, 1}, 1, false);
        uploadImage(whiteAo.image, {1, 1}, 1, 1, sizeof(float), {&one});
        whiteAoSet = allocateSet(aoPool, aoSampleLayout);
        for (uint32_t b = 0; b < 3; ++b)
            writeImage(whiteAoSet, b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, whiteAo.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void destroyAo()
    {
        destroyImage(whiteAo);
        if (pointSampler)
            vkDestroySampler(vk.device, pointSampler, nullptr);
        if (aoPool)
            vkDestroyDescriptorPool(vk.device, aoPool, nullptr);
        for (VkDescriptorSetLayout* l : {&aoSampleLayout, &aoComputeLayout})
            if (*l)
            {
                vkDestroyDescriptorSetLayout(vk.device, *l, nullptr);
                *l = VK_NULL_HANDLE;
            }
        pointSampler = VK_NULL_HANDLE;
        aoPool = VK_NULL_HANDLE;
    }

    /** Sized with the swapchain. The two AO images stay in GENERAL so compute can write and shaders can sample them. */
    void createAoImages(Viewport& v, VkExtent2D extent)
    {
        LILA_ZONE();

        constexpr auto sampled = VK_IMAGE_USAGE_SAMPLED_BIT;
        v.aoDepth = createImage(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | sampled, extent, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
        v.aoNormal = createImage(kAoNormalFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled, extent, VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
        v.aoRaw = createImage(kAoFormat, VK_IMAGE_USAGE_STORAGE_BIT | sampled, extent, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        v.aoFinal = createImage(kAoFormat, VK_IMAGE_USAGE_STORAGE_BIT | sampled, extent, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

        constexpr auto sampler = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        constexpr auto storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        constexpr auto depthRead = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        constexpr auto shaderRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        constexpr auto general = VK_IMAGE_LAYOUT_GENERAL;
        for (VkDescriptorSet set : {v.gtaoSet, v.denoiseSet})
        {
            writeImage(set, 0, sampler, v.aoDepth.view, depthRead);
            writeImage(set, 1, sampler, v.aoNormal.view, shaderRead);
        }
        writeImage(v.gtaoSet, 2, sampler, v.aoFinal.view, general);
        writeImage(v.gtaoSet, 3, storage, v.aoRaw.view, general);
        writeImage(v.denoiseSet, 2, sampler, v.aoRaw.view, general);
        writeImage(v.denoiseSet, 3, storage, v.aoFinal.view, general);
        writeImage(v.aoSampleSet, 0, sampler, v.aoFinal.view, general);
        writeImage(v.aoSampleSet, 1, sampler, v.aoDepth.view, depthRead);
        writeImage(v.aoSampleSet, 2, sampler, v.aoNormal.view, shaderRead);

        v.postColor = createImage(v.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled, extent, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        writeImage(v.postSet, 0, sampler, v.postColor.view, shaderRead);
        writeImage(v.postSet, 1, sampler, v.aoDepth.view, depthRead);
        writeImage(v.postSet, 2, sampler, v.aoNormal.view, shaderRead);
    }

    static bool anyShaded(const FrameState& s)
    {
        return std::ranges::any_of(s.meshes, [](const MeshDraw& d) { return (d.flags & DrawShaded) && d.mesh && !d.mesh->indices.empty(); });
    }

    /**
     * Records the depth/normal prepass and both AO dispatches ahead of the main pass, leaving v.aoFinal ready for the
     * mesh shader. The images are shared by both frames in flight, so each first use waits on the previous frame's reads.
     */
    void recordAo(VkCommandBuffer cmd, Viewport& v, const FrameState& s, const glm::mat4& viewProj, bool gtao = true)
    {
        LILA_ZONE();

        constexpr auto compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        constexpr auto fragment = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        constexpr auto colorStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        constexpr auto depthStage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr auto depthWrite = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        constexpr auto read = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        constexpr auto color = VK_IMAGE_ASPECT_COLOR_BIT;

        barrier(cmd, v.aoDepth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, compute | fragment, 0, depthStage,
                depthWrite, VK_IMAGE_ASPECT_DEPTH_BIT);
        barrier(cmd, v.aoNormal.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, compute | fragment, 0, colorStage,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, color);

        VkRenderingAttachmentInfo normal{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        normal.imageView = v.aoNormal.view;
        normal.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        normal.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        normal.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = v.aoDepth.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {0.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, v.extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &normal;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport viewport{0, 0, float(v.extent.width), float(v.extent.height), 0, 1};
        VkRect2D scissor{{0, 0}, v.extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prepassPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &envSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 2, 1, &whiteAoSet, 0, nullptr);
        VkDescriptorSet boundSet = VK_NULL_HANDLE;
        for (const DrawBatch& b : frameBatches.shaded)
        {
            const MeshDraw& d = *b.draw;
            GpuMesh* g = gpuMesh(d.mesh);
            for (const MeshSection& sec : sectionsOf(d, *g))
            {
                const Material* mat = sec.material < d.materials.size() ? &d.materials[sec.material] : nullptr;
                if (!mat || !(mat->flags & MatTranslucent))
                    drawSection(cmd, s, b, *g, sec, viewProj, boundSet);
            }
        }
        vkCmdEndRendering(cmd);

        barrier(cmd, v.aoDepth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, depthStage,
                depthWrite, compute | fragment, read, VK_IMAGE_ASPECT_DEPTH_BIT);
        barrier(cmd, v.aoNormal.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, colorStage,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, compute | fragment, read, color);
        if (gtao)
            recordGtao(cmd, v, s, false);
    }

    /** Fast AO: GTAO from the main pass's depth (resolved into aoDepth) with normals taken from it, ready for applyAo. */
    void recordFastAo(VkCommandBuffer cmd, Viewport& v, const FrameState& s)
    {
        LILA_ZONE();

        constexpr auto compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        constexpr auto depthStage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        // A depth resolve writes in the colour attachment output stage.
        barrier(cmd, v.aoDepth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                depthStage | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, compute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT);
        barrier(cmd, v.aoNormal.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, compute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                0, compute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        recordGtao(cmd, v, s, true);
    }

    /** Multiplies the frame's meshes by the fast AO, in a rendering pass that continues the main one. */
    void applyAo(VkCommandBuffer cmd, Viewport& v)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, aoApplyPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, aoApplyLayout, 0, 1, &v.aoSampleSet, 0, nullptr);
        // Applied to the display-encoded colour as is, which darkens about as much as the accurate mode does.
        const glm::vec4 push(1.0f, 0.0f, 0.0f, 0.0f);
        vkCmdPushConstants(cmd, aoApplyLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    /** GTAO and its blur from aoDepth (and aoNormal unless depthNormals), leaving aoFinal ready for fragment shaders. */
    void recordGtao(VkCommandBuffer cmd, Viewport& v, const FrameState& s, bool depthNormals)
    {
        constexpr auto compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        constexpr auto fragment = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        constexpr auto read = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        constexpr auto storageWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        constexpr auto general = VK_IMAGE_LAYOUT_GENERAL;
        constexpr auto color = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier(cmd, v.aoRaw.image, VK_IMAGE_LAYOUT_UNDEFINED, general, compute, 0, compute, storageWrite, color);

        glm::dmat4 proj = s.proj;
        AoPush p;
        p.invProj = glm::mat4(glm::inverse(proj));
        p.view = glm::mat4(s.view);
        bool ortho = proj[3][3] == 1.0;
        p.params = glm::vec4(std::max(s.aoRadius, 0.01f), std::max(s.aoIntensity, 0.0f), float(proj[0][0] * 0.5 * v.extent.width),
                             ortho ? 1.0f : 0.0f);
        p.size = glm::vec4(float(v.extent.width), float(v.extent.height), 1.0f / float(v.extent.width), 1.0f / float(v.extent.height));
        p.options.x = depthNormals ? 1.0f : 0.0f;
        uint32_t gx = (v.extent.width + 7) / 8, gy = (v.extent.height + 7) / 8;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoLayout, 0, 1, &v.gtaoSet, 0, nullptr);
        vkCmdPushConstants(cmd, aoLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, gx, gy, 1);

        barrier(cmd, v.aoRaw.image, general, general, compute, storageWrite, compute, read, color);
        barrier(cmd, v.aoFinal.image, VK_IMAGE_LAYOUT_UNDEFINED, general, fragment, 0, compute, storageWrite, color);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoisePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoLayout, 0, 1, &v.denoiseSet, 0, nullptr);
        vkCmdPushConstants(cmd, aoLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, gx, gy, 1);
        barrier(cmd, v.aoFinal.image, general, general, compute, storageWrite, fragment, read, color);
    }

    // ---------------------------------------------------------------- pipelines

    VkShaderModule module(const std::vector<uint32_t>& code) { return compute::shaderModule(vk, code); }

    /** A requested line width within what the device rasterizes. */
    float lineWidth(float w) const
    {
        const float* range = vk.properties.limits.lineWidthRange;
        return std::clamp(w, std::max(1.0f, range[0]), std::max(1.0f, range[1]));
    }

    VkPipeline buildPipeline(VkPipelineLayout layout, VkShaderModule mod, const char* vs, const char* ps, VkFormat format,
                             VkPrimitiveTopology topology, const VkPipelineVertexInputStateCreateInfo& vertexInput, bool depthWrite,
                             bool blend, bool depthTest = true, float depthBias = 0.0f, bool dynamicCull = false, bool singleSample = false,
                             bool dynamicLineWidth = false, const VkPipelineColorBlendAttachmentState* blendState = nullptr)
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, mod, vs};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mod, ps};

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = topology;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        // Reversed-Z: a positive bias pulls toward the camera, which keeps wireframes on top of their own faces.
        rs.depthBiasEnable = depthBias != 0.0f;
        rs.depthBiasConstantFactor = depthBias;
        rs.depthBiasSlopeFactor = depthBias != 0.0f ? 1.0f : 0.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = singleSample ? VK_SAMPLE_COUNT_1_BIT : vk.msaa;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = depthTest;
        ds.depthWriteEnable = depthWrite;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed-Z
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = 0xF;
        if (blend)
        {
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp = VK_BLEND_OP_ADD;
            att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        if (blendState)
            att = *blendState;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        uint32_t dynCount = 2;
        if (dynamicCull)
            dyn[dynCount++] = VK_DYNAMIC_STATE_CULL_MODE;
        if (dynamicLineWidth)
            dyn[dynCount++] = VK_DYNAMIC_STATE_LINE_WIDTH;
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = dynCount;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &format;
        ri.depthAttachmentFormat = kDepthFormat;

        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.pNext = &ri;
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = &vertexInput;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &cb;
        pci.pDynamicState = &dy;
        pci.layout = layout;
        VkPipeline p = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pci, nullptr, &p) != VK_SUCCESS)
            log::error("render: pipeline creation failed ({} / {})", vs, ps);
        return p;
    }

    VkPipeline buildCompute(VkPipelineLayout layout, VkShaderModule mod, const char* entry)
    {
        VkPipeline p = compute::computePipeline(vk, layout, mod, entry);
        if (!p)
            log::error("render: compute pipeline creation failed ({})", entry);
        return p;
    }

    void createPipelines(VkFormat format)
    {
        LILA_ZONE();

        destroyPipelines();
        pipelineFormat = format;

        auto layoutFor = [&](uint32_t size, VkShaderStageFlags stages, VkDescriptorSetLayout set = VK_NULL_HANDLE) {
            return compute::pipelineLayout(vk, size, stages, set ? std::span(&set, 1) : std::span<const VkDescriptorSetLayout>());
        };
        gridLayout = layoutFor(sizeof(GridPush), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        linesLayout = layoutFor(sizeof(LinesPush), VK_SHADER_STAGE_VERTEX_BIT);
        pointsLayout = layoutFor(sizeof(PointsPush), VK_SHADER_STAGE_VERTEX_BIT);
        overlayTrisLayout = layoutFor(sizeof(glm::vec2), VK_SHADER_STAGE_VERTEX_BIT, atlasSetLayout);

        VkShaderModule grid = module(loadSpirv("grid.spv"));
        VkShaderModule lines = module(loadSpirv("lines.spv"));

        VkPipelineVertexInputStateCreateInfo none{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (grid)
            gridPipeline = buildPipeline(gridLayout, grid, "gridVS", "gridPS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, none, false, true);

        VkVertexInputBindingDescription binding{0, sizeof(LineVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[2] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LineVertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(LineVertex, color)},
        };
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &binding;
        vin.vertexAttributeDescriptionCount = 2;
        vin.pVertexAttributeDescriptions = attrs;
        if (lines)
        {
            linesPipeline = buildPipeline(linesLayout, lines, "linesVS", "linesPS", format, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, vin, true, false);
            overlayPipeline =
                buildPipeline(linesLayout, lines, "linesVS", "linesPS", format, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, vin, false, true, false);
            linesOnTopPipeline = buildPipeline(linesLayout, lines, "linesVS", "linesPS", format, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, vin, false,
                                               true, true, 0.0f, false, false, true);
            tris3dPipeline =
                buildPipeline(linesLayout, lines, "linesVS", "linesPS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, vin, false, true, true, 2.0f);
            linesOverPipeline = buildPipeline(linesLayout, lines, "linesVS", "linesPS", format, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, vin, false,
                                              true, false, 0.0f, false, false, true);
            if (VkShaderModule points = module(loadSpirv("points.spv")))
            {
                VkVertexInputBindingDescription pb{0, sizeof(PointVertex), VK_VERTEX_INPUT_RATE_VERTEX};
                VkVertexInputAttributeDescription pa[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PointVertex, pos)},
                                                           {1, 0, VK_FORMAT_R32_UINT, offsetof(PointVertex, state)}};
                VkPipelineVertexInputStateCreateInfo pvin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                pvin.vertexBindingDescriptionCount = 1;
                pvin.pVertexBindingDescriptions = &pb;
                pvin.vertexAttributeDescriptionCount = 2;
                pvin.pVertexAttributeDescriptions = pa;
                pointsPipeline =
                    buildPipeline(pointsLayout, points, "pointsVS", "pointsPS", format, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, pvin, false, false, true);
                vkDestroyShaderModule(vk.device, points, nullptr);
            }
        }

        {
            const VkDescriptorSetLayout sets[3] = {envSetLayout, bindless ? bindlessLayout : materialSetLayout, aoSampleLayout};
            meshLayout = compute::pipelineLayout(vk, sizeof(MeshPush), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sets);
        }
        wireLayout = layoutFor(sizeof(WirePush), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        if (VkShaderModule mesh = module(loadSpirv(bindless ? "mesh_bindless.spv" : "mesh.spv")))
        {
            VkVertexInputBindingDescription mb{0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription ma[6] = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)},
                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)},
                {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, tangent)},
                {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)},
                {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv1)},
                {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, color)},
            };
            VkPipelineVertexInputStateCreateInfo mvin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            mvin.vertexBindingDescriptionCount = 1;
            mvin.pVertexBindingDescriptions = &mb;
            mvin.vertexAttributeDescriptionCount = 6;
            mvin.pVertexAttributeDescriptions = ma;
            meshPipeline =
                buildPipeline(meshLayout, mesh, "meshVS", "meshPS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, mvin, true, false, true, 0.0f, true);
            meshBlendPipeline =
                buildPipeline(meshLayout, mesh, "meshVS", "meshPS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, mvin, false, true, true, 0.0f, true);
            prepassPipeline = buildPipeline(meshLayout, mesh, "meshVS", "prepassPS", kAoNormalFormat, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, mvin, true,
                                            false, true, 0.0f, true, true);
            if (vk.fragmentBarycentric)
                if (VkShaderModule wired = module(loadSpirv(bindless ? "mesh_bindless_wire.spv" : "mesh_wire.spv")))
                {
                    meshWirePipeline = buildPipeline(meshLayout, wired, "meshVS", "meshWirePS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, mvin, true,
                                                     false, true, 0.0f, true);
                    meshWireBlendPipeline = buildPipeline(meshLayout, wired, "meshVS", "meshWirePS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, mvin,
                                                          false, true, true, 0.0f, true);
                    vkDestroyShaderModule(vk.device, wired, nullptr);
                }
            backgroundPipeline = buildPipeline(meshLayout, mesh, "backgroundVS", "backgroundPS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                               none, false, false, false);
            vkDestroyShaderModule(vk.device, mesh, nullptr);
        }
        aoLayout = layoutFor(sizeof(AoPush), VK_SHADER_STAGE_COMPUTE_BIT, aoComputeLayout);
        if (VkShaderModule ao = module(loadSpirv("ao.spv")))
        {
            gtaoPipeline = buildCompute(aoLayout, ao, "gtaoCS");
            denoisePipeline = buildCompute(aoLayout, ao, "denoiseCS");
            vkDestroyShaderModule(vk.device, ao, nullptr);
        }
        aoApplyLayout = layoutFor(sizeof(glm::vec4), VK_SHADER_STAGE_FRAGMENT_BIT, aoSampleLayout);
        if (VkShaderModule apply = module(loadSpirv("ao_apply.spv")))
        {
            VkPipelineColorBlendAttachmentState multiply{};
            multiply.blendEnable = VK_TRUE;
            multiply.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            multiply.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            multiply.colorBlendOp = VK_BLEND_OP_ADD;
            multiply.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            multiply.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            multiply.alphaBlendOp = VK_BLEND_OP_ADD;
            multiply.colorWriteMask = 0xF;
            aoApplyPipeline = buildPipeline(aoApplyLayout, apply, "applyVS", "applyPS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, none, false,
                                            false, false, 0.0f, false, false, false, &multiply);
            vkDestroyShaderModule(vk.device, apply, nullptr);
        }
        postLayout = layoutFor(sizeof(PostPush), VK_SHADER_STAGE_FRAGMENT_BIT, aoSampleLayout);
        if (VkShaderModule post = module(loadSpirv("post.spv")))
        {
            postPipeline = buildPipeline(postLayout, post, "postVS", "postPS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, none, false, false, false);
            vkDestroyShaderModule(vk.device, post, nullptr);
        }
        contactLayout = layoutFor(sizeof(ContactPush), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        if (VkShaderModule contact = module(loadSpirv("contact.spv")))
        {
            contactPipeline = buildPipeline(contactLayout, contact, "contactVS", "contactPS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, none,
                                            false, true);
            vkDestroyShaderModule(vk.device, contact, nullptr);
        }
        if (VkShaderModule wire = module(loadSpirv("wire.spv")))
        {
            VkVertexInputBindingDescription wb{0, sizeof(glm::vec3), VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription wa{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
            VkPipelineVertexInputStateCreateInfo wvin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            wvin.vertexBindingDescriptionCount = 1;
            wvin.pVertexBindingDescriptions = &wb;
            wvin.vertexAttributeDescriptionCount = 1;
            wvin.pVertexAttributeDescriptions = &wa;
            wirePipeline = buildPipeline(wireLayout, wire, "wireVS", "wirePS", format, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, wvin, false, true, true, 0.0f,
                                         false, false, true);
            wireXrayPipeline = buildPipeline(wireLayout, wire, "wireVS", "wirePS", format, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, wvin, false, true, false,
                                             0.0f, false, false, true);
            vkDestroyShaderModule(vk.device, wire, nullptr);
        }

        if (VkShaderModule overlay = module(loadSpirv("overlay.spv")))
        {
            VkVertexInputBindingDescription ob{0, sizeof(OverlayVertex), VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription oa[3] = {
                {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, pos)},
                {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, uv)},
                {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(OverlayVertex, color)},
            };
            VkPipelineVertexInputStateCreateInfo ovin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            ovin.vertexBindingDescriptionCount = 1;
            ovin.pVertexBindingDescriptions = &ob;
            ovin.vertexAttributeDescriptionCount = 3;
            ovin.pVertexAttributeDescriptions = oa;
            overlayTrisPipeline = buildPipeline(overlayTrisLayout, overlay, "overlayVS", "overlayPS", format,
                                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, ovin, false, true, false);
            vkDestroyShaderModule(vk.device, overlay, nullptr);
        }

        imagePlaneLayout = layoutFor(sizeof(ImagePlanePush), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, materialSetLayout);
        if (VkShaderModule plane = module(loadSpirv("imageplane.spv")))
        {
            auto build = [&](bool depthWrite, bool depthTest) {
                return buildPipeline(imagePlaneLayout, plane, "imagePlaneVS", "imagePlanePS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, none,
                                     depthWrite, true, depthTest);
            };
            imageOverPipeline = build(false, false);
            imageDepthPipeline = build(true, true);
            imageBlendPipeline = build(false, true);
            vkDestroyShaderModule(vk.device, plane, nullptr);
        }

        if (grid)
            vkDestroyShaderModule(vk.device, grid, nullptr);
        if (lines)
            vkDestroyShaderModule(vk.device, lines, nullptr);
    }

    void destroyPipelines()
    {
        for (VkPipeline* p : {&gridPipeline, &linesPipeline, &overlayPipeline, &overlayTrisPipeline, &meshPipeline, &meshBlendPipeline, &meshWirePipeline,
                              &meshWireBlendPipeline, &aoApplyPipeline, &pointsPipeline, &wirePipeline,
                              &wireXrayPipeline, &backgroundPipeline, &linesOnTopPipeline, &tris3dPipeline, &linesOverPipeline, &contactPipeline, &prepassPipeline, &gtaoPipeline,
                              &denoisePipeline, &imageOverPipeline, &imageDepthPipeline, &imageBlendPipeline, &postPipeline})
            if (*p)
            {
                vkDestroyPipeline(vk.device, *p, nullptr);
                *p = VK_NULL_HANDLE;
            }
        for (VkPipelineLayout* l : {&gridLayout, &linesLayout, &overlayTrisLayout, &meshLayout, &wireLayout, &contactLayout, &aoLayout, &aoApplyLayout, &pointsLayout,
                                    &imagePlaneLayout, &postLayout})
            if (*l)
            {
                vkDestroyPipelineLayout(vk.device, *l, nullptr);
                *l = VK_NULL_HANDLE;
            }
    }

    // ---------------------------------------------------------------- frame

    static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                        VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, VkImageAspectFlags aspect,
                        uint32_t mips = 1, uint32_t layers = 1)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.image = image;
        b.subresourceRange = {aspect, 0, mips, 0, layers};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    }

    /** Switches every viewport to the frame's sample count: rebuilds the pipelines now, the targets on each next frame. */
    void applyMsaa(const FrameState& s)
    {
        auto want = VkSampleCountFlagBits(msaaSamples(true, s.msaaSamples, vk.msaaSupported));
        if (want == vk.msaa)
            return;
        LILA_ZONE();
        vkDeviceWaitIdle(vk.device);
        vk.msaa = want;
        ++msaaGeneration;
        if (pipelineFormat != VK_FORMAT_UNDEFINED)
            createPipelines(pipelineFormat);
        log::info("render: {}x MSAA", int(want));
    }

    /**
     * Mailbox where the surface has it: the display takes the newest finished frame at each vblank, so a camera drag
     * shows within a vblank of the input instead of behind FIFO's queue of older frames, and a view never blocks the
     * render thread (and the other views) waiting for its vblank. Drawing is on demand, so the GPU only runs ahead
     * while something changes.
     */
    VkPresentModeKHR presentMode(VkSurfaceKHR surface)
    {
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, surface, &n, modes.data());
        bool mailbox = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end();
        if (!presentModeLogged)
        {
            presentModeLogged = true;
            log::info("render: {} presentation", mailbox ? "mailbox" : "fifo");
        }
        return mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    }
    bool presentModeLogged = false;

    /** With the frame's fence signalled: what its timestamps measured. */
    void readTimings(Viewport& v, Frame& f)
    {
        if (!f.timestamps || !f.timed)
            return;
        f.timed = false;
        uint64_t t[4] = {};
        if (vkGetQueryPoolResults(vk.device, f.timestamps, 0, 4, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
        double toMs = double(vk.properties.limits.timestampPeriod) * 1e-6;
        v.timing.gpuMs = double(t[3] - t[0]) * toMs;
        v.timing.aoMs = f.aoTimed ? double(t[2] - t[1]) * toMs : 0.0;
    }

    void timestamp(VkCommandBuffer cmd, Frame& f, uint32_t query)
    {
        if (f.timestamps)
            vkCmdWriteTimestamp2(cmd, query == 0 ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f.timestamps, query);
    }

    /**
     * True when the frame could not be shown and should be drawn again straight away. latch runs once the frame's
     * fence and swapchain image are in hand, and may swap in a newer state submitted while the thread was blocked.
     */
    bool render(Viewport& v, FrameState& s, const std::function<void(FrameState&)>& latch = {})
    {
        LILA_ZONE();

        applyMsaa(s);
        if (v.msaaGeneration != msaaGeneration)
            v.needsRecreate = true;

        // Not every driver reports a child window's resize through OUT_OF_DATE, so compare against the window itself.
        if (v.swapchain && !v.needsRecreate && !v.offscreen)
        {
            VkSurfaceCapabilitiesKHR caps;
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, v.surface, &caps) == VK_SUCCESS && caps.currentExtent.width != UINT32_MAX &&
                (caps.currentExtent.width != v.extent.width || caps.currentExtent.height != v.extent.height))
                v.needsRecreate = true;
        }
        if (v.needsRecreate.exchange(false) && !createSwapchain(v))
        {
            v.needsRecreate = true;
            return false;
        }

        Frame& f = v.frames[v.frameIndex];
        vkWaitForFences(vk.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
        f.feedback.collect();
        freeStaging(f);
        readTimings(v, f);
        GpuImage::collectGarbage(vk);

        uint32_t imageIndex = 0;
        VkResult r = v.offscreen ? VK_SUCCESS : vkAcquireNextImageKHR(vk.device, v.swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imageIndex);
        if (r == VK_ERROR_OUT_OF_DATE_KHR)
        {
            v.needsRecreate = true;
            return true;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        {
            log::error("render: acquire failed ({})", vkResultName(r));
            return false;
        }
        vkResetFences(vk.device, 1, &f.fence);
        // Waiting above can take a whole vblank; draw what the user is doing now, not what they did before it.
        if (latch)
            latch(s);

        if (s.atlas && s.atlas.get() != uploadedAtlas)
            uploadAtlas(s.atlas);

        // One buffer per frame: lines, linesOnTop, tris3d, overlay and linesXray share the LineVertex stride, overlayTris follow.
        const std::vector<LineVertex>* lineSets[] = {&s.lines, &s.linesOnTop, &s.tris3d, &s.overlay, &s.linesXray, &s.selectedEdges, &s.seamLines,
                                                     &s.symmetryLines, &s.highlightEdges, &s.curveHalo, &s.curveLines, &s.curveHaloBold, &s.curveLinesBold};
        constexpr int kLineSets = int(std::size(lineSets));
        uint32_t lineFirst[kLineSets];
        size_t lineBytes = 0;
        for (int i = 0; i < kLineSets; ++i)
        {
            lineFirst[i] = uint32_t(lineBytes / sizeof(LineVertex));
            lineBytes += lineSets[i]->size() * sizeof(LineVertex);
        }
        // Debug drawing follows the fixed sets: each line batch, then its triangles.
        const size_t debugBatches = s.debugLines ? s.debugLines->size() : 0;
        std::vector<uint32_t> debugFirst(debugBatches + 1);
        for (size_t i = 0; i <= debugBatches; ++i)
        {
            debugFirst[i] = uint32_t(lineBytes / sizeof(LineVertex));
            const std::vector<LineVertex>* verts = i < debugBatches ? &(*s.debugLines)[i].vertices : s.debugTris.get();
            lineBytes += verts ? verts->size() * sizeof(LineVertex) : 0;
        }
        size_t trisBytes = s.overlayTris.size() * sizeof(OverlayVertex);
        if (lineBytes + trisBytes)
        {
            ensureLines(f, lineBytes + trisBytes);
            LineVertex* mapped = static_cast<LineVertex*>(f.linesMapped);
            const glm::dmat4 clipViewProj = s.proj * s.view;
            const glm::vec3 behind(s.eye + glm::dvec3(s.view[0][2], s.view[1][2], s.view[2][2]));
            for (int i = 0; i < kLineSets; ++i)
            {
                std::memcpy(mapped + lineFirst[i], lineSets[i]->data(), lineSets[i]->size() * sizeof(LineVertex));
                // Every set but tris3d (triangles) and overlay (already in NDC) is a world-space line list.
                if (i != 2 && i != 3)
                    clipLineList(lineSets[i]->data(), mapped + lineFirst[i], lineSets[i]->size(), clipViewProj, behind);
            }
            for (size_t i = 0; i <= debugBatches; ++i)
                if (const std::vector<LineVertex>* verts = i < debugBatches ? &(*s.debugLines)[i].vertices : s.debugTris.get())
                {
                    std::memcpy(mapped + debugFirst[i], verts->data(), verts->size() * sizeof(LineVertex));
                    if (i < debugBatches)
                        clipLineList(verts->data(), mapped + debugFirst[i], verts->size(), clipViewProj, behind);
                }
            std::memcpy(static_cast<char*>(f.linesMapped) + lineBytes, s.overlayTris.data(), trisBytes);
        }

        VkCommandBuffer cmd = f.cmd;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        const auto cpuStart = std::chrono::steady_clock::now();
        if (f.timestamps)
        {
            vkCmdResetQueryPool(cmd, f.timestamps, 0, 4);
            timestamp(cmd, f, 0);
            f.timed = true;
        }
        ++frameCounter;
        prepareTextures(cmd, f, s);
        if (frameUsesVt && frameTableAddress && f.feedback.begin(vk, cmd, int(v.extent.width), int(v.extent.height), frameCounter))
        {
            const std::array<uint32_t, vt::kTableHeaderWords> header = f.feedback.header();
            std::memcpy(f.table.mapped, header.data(), sizeof(header));
            vmaFlushAllocation(vk.allocator, f.table.alloc, 0, sizeof(header));
        }
        else if (f.feedback.active())
            f.feedback.destroy(vk);
        prepareMeshes(cmd, f, s);
        prepareDynamicMeshes(cmd, f, s);
        writeInstances(f, s);
        frameMeshDraws = frameMeshInstances = 0;

        glm::dmat4 viewProj = s.proj * s.view;
        glm::mat4 vpF(viewProj);
        bool ao = s.ambientOcclusion && s.aoIntensity > 0.0f && prepassPipeline && gtaoPipeline && denoisePipeline && v.gtaoSet && v.denoiseSet &&
                  v.aoSampleSet && anyShaded(s) && !s.xray;
        const bool post = (s.kuwahara || s.outline) && postPipeline && v.postSet && v.postColor.view;
        // Fast AO comes from the main pass's own depth after the meshes; the post pass reads the prepass, so it keeps it.
        const bool aoFast = ao && s.aoFast && !post && aoApplyPipeline;
        f.aoTimed = ao && !aoFast;
        const bool prepass = (ao && !aoFast) || (post && s.outline && prepassPipeline && anyShaded(s) && !s.xray);
        timestamp(cmd, f, 1);
        const auto aoStart = std::chrono::steady_clock::now();
        if (prepass)
            recordAo(cmd, v, s, vpF, ao && !aoFast);
        else if (post)
        {
            // Nothing wrote them this frame; the post set still needs them in their sampled layouts.
            barrier(cmd, v.aoDepth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
            barrier(cmd, v.aoNormal.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        v.timing.aoCpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - aoStart).count();
        timestamp(cmd, f, 2);

        constexpr auto colorStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        constexpr auto colorWrite = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        constexpr auto depthStage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr auto depthAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        barrier(cmd, v.images[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorStage, 0, colorStage,
                colorWrite, VK_IMAGE_ASPECT_COLOR_BIT);
        if (v.color.image)
            barrier(cmd, v.color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorStage, colorWrite, colorStage,
                    colorWrite, VK_IMAGE_ASPECT_COLOR_BIT);
        barrier(cmd, v.depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depthStage, depthAccess, depthStage,
                depthAccess, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (post)
            barrier(cmd, v.postColor.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    colorStage | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, colorStage, colorWrite, VK_IMAGE_ASPECT_COLOR_BIT);

        glm::vec3 bg = srgbToLinear(s.background);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = v.color.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.clearValue.color = {{bg.r, bg.g, bg.b, 1.0f}};
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = v.views[imageIndex];
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (!v.color.image)
        {
            color.imageView = v.views[imageIndex];
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.resolveMode = VK_RESOLVE_MODE_NONE;
            color.resolveImageView = VK_NULL_HANDLE;
        }
        // With post on, the scene resolves into postColor; the frame's own image is written by the post pass.
        const VkRenderingAttachmentInfo frameColor = color;
        if (post)
        {
            if (v.color.image)
                color.resolveImageView = v.postColor.view;
            else
                color.imageView = v.postColor.view;
        }
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = v.depth.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = post ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {0.0f, 0}; // reversed-Z
        // Fast AO ends the pass after the meshes and picks it up again with these once the AO is applied.
        VkRenderingAttachmentInfo resumeColor = color, resumeDepth = depth;
        if (aoFast)
        {
            resumeColor.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            resumeDepth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.resolveMode = VK_RESOLVE_MODE_NONE;
            color.resolveImageView = VK_NULL_HANDLE;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            barrier(cmd, v.aoDepth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, depthStage | colorStage, depthAccess | colorWrite,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
            if (v.color.image)
            {
                depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                depth.resolveImageView = v.aoDepth.view;
                depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            }
            else
                depth.imageView = resumeDepth.imageView = v.aoDepth.view;
        }

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, v.extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);

        VkViewport viewport{0, 0, float(v.extent.width), float(v.extent.height), 0, 1};
        VkRect2D scissor{{0, 0}, v.extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        if (backgroundPipeline && envLoaded && s.envBackground)
        {
            MeshPush p{};
            p.viewProj = glm::mat4(glm::inverse(viewProj));
            p.base = glm::vec4(std::clamp(s.envBlur, 0.0f, 1.0f) * envMaxLod, std::clamp(s.envSaturation, 0.0f, 1.0f),
                               s.envBackgroundExposure, 0.0f);
            p.params = glm::vec4(s.exposure, s.zUp ? 1.0f : 0.0f, 0.0f, 1.0f);
            p.env = envParams(s);
            p.gamma = s.gamma;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, backgroundPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &envSet, 0, nullptr);
            vkCmdPushConstants(cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

        // Grid next: it writes no depth, so anything on the ground plane draws over it.
        if (gridPipeline && s.grid)
        {
            GridPush gp;
            gp.viewProj = vpF;
            gp.invViewProj = glm::mat4(glm::inverse(viewProj));
            gp.eye = glm::vec4(glm::vec3(s.eye), 1.0f);
            gp.params = glm::vec4(s.gridSpacing, float(s.gridMajorEvery), s.zUp ? 1.0f : 0.0f, 0.0f);
            gp.color = glm::vec4(s.gridColor, 1.0f);
            gp.axisX = glm::vec4(s.gridAxisX, 1.0f);
            gp.axisZ = glm::vec4(s.gridAxisZ, 1.0f);
            gp.origin = glm::vec4(glm::vec3(s.gridOrigin), 0.0f);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipeline);
            vkCmdPushConstants(cmd, gridLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GridPush), &gp);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

        drawImagePlanes(cmd, s, vpF, ImagePlaneDraw::Depth::Behind, false);
        drawContactShadows(cmd, s, vpF);
        // Opaque planes in the scene write depth before the meshes; see-through ones blend over them afterwards.
        drawImagePlanes(cmd, s, vpF, ImagePlaneDraw::Depth::Scene, false);
        drawMeshes(cmd, s, vpF, ao && !aoFast ? v.aoSampleSet : whiteAoSet, v.extent.height);
        drawImagePlanes(cmd, s, vpF, ImagePlaneDraw::Depth::Scene, true);
        drawImagePlanes(cmd, s, vpF, ImagePlaneDraw::Depth::Front, false);
        if (aoFast)
        {
            vkCmdEndRendering(cmd);
            recordFastAo(cmd, v, s);
            if (!v.color.image)
                barrier(cmd, v.aoDepth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, depthStage, depthAccess, VK_IMAGE_ASPECT_DEPTH_BIT);
            ri.pColorAttachments = &resumeColor;
            ri.pDepthAttachment = &resumeDepth;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            applyAo(cmd, v);
        }
        if (post)
            recordPost(cmd, v, s, ri, frameColor, depth);

        if (linesPipeline && !s.lines.empty())
        {
            const LinesPush lp{vpF};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, linesPipeline);
            vkCmdPushConstants(cmd, linesLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(lp), &lp);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &f.lines, &offset);
            vkCmdDraw(cmd, uint32_t(s.lines.size()), 1, 0, 0);
        }

        // Selected edges draw between the other on-top lines and tris3d, so hover ribbons stay over them.
        struct Biased
        {
            VkPipeline pipeline;
            int set;
            float width;
        };
        const Biased biased[] = {{linesOnTopPipeline, 9, s.curveHaloWidth},     {linesOnTopPipeline, 10, s.curveWidth},
                                 {linesOnTopPipeline, 11, s.curveHaloBoldWidth}, {linesOnTopPipeline, 12, s.curveBoldWidth},
                                 {linesOnTopPipeline, 1, 1.0f}, {linesOnTopPipeline, 8, s.highlightEdgeWidth}, {linesOnTopPipeline, 6, s.seamLineWidth},
                                 {linesOnTopPipeline, 7, s.symmetryLineWidth}, {linesOnTopPipeline, 5, s.selectedEdgeWidth},
                                 {tris3dPipeline, 2, 0.0f}};
        drawPoints(cmd, s, vpF, v.extent.height);
        for (const Biased& b : biased)
        {
            const std::vector<LineVertex>& verts = *lineSets[b.set];
            if (!b.pipeline || verts.empty())
                continue;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, b.pipeline);
            if (b.width > 0.0f)
                vkCmdSetLineWidth(cmd, lineWidth(b.width));
            const LinesPush lp{vpF, depthPull(s.proj, v.extent.height, kLinePullPixels + 0.5 * lineWidth(std::max(b.width, 1.0f)))};
            vkCmdPushConstants(cmd, linesLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(lp), &lp);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &f.lines, &offset);
            vkCmdDraw(cmd, uint32_t(verts.size()), 1, lineFirst[b.set], 0);
        }

        // Debug drawing: depth-tested lines and triangles here, the on-top lines over the x-ray joints.
        auto drawDebugLines = [&](bool onTop) {
            for (size_t i = 0; i < debugBatches; ++i)
            {
                const LineBatch& batch = (*s.debugLines)[i];
                VkPipeline pipeline = onTop ? linesOverPipeline : linesOnTopPipeline;
                if (batch.onTop != onTop || batch.vertices.empty() || !pipeline)
                    continue;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdSetLineWidth(cmd, lineWidth(batch.width));
                const LinesPush lp{vpF, onTop ? glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)
                                              : depthPull(s.proj, v.extent.height, kLinePullPixels + 0.5 * lineWidth(batch.width))};
                vkCmdPushConstants(cmd, linesLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(lp), &lp);
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &f.lines, &offset);
                vkCmdDraw(cmd, uint32_t(batch.vertices.size()), 1, debugFirst[i], 0);
            }
        };
        if (s.debugTris && !s.debugTris->empty() && tris3dPipeline)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tris3dPipeline);
            const LinesPush lp{vpF, depthPull(s.proj, v.extent.height, kLinePullPixels)};
            vkCmdPushConstants(cmd, linesLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(lp), &lp);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &f.lines, &offset);
            vkCmdDraw(cmd, uint32_t(s.debugTris->size()), 1, debugFirst[debugBatches], 0);
        }
        drawDebugLines(false);

        if (overlayPipeline && !s.linesXray.empty())
        {
            const LinesPush lp{vpF};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline);
            vkCmdPushConstants(cmd, linesLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(lp), &lp);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &f.lines, &offset);
            vkCmdDraw(cmd, uint32_t(s.linesXray.size()), 1, lineFirst[4], 0);
        }
        drawDebugLines(true);

        if (overlayPipeline && !s.overlay.empty())
        {
            const LinesPush lp{glm::mat4(1.0f)};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline);
            vkCmdPushConstants(cmd, linesLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(lp), &lp);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &f.lines, &offset);
            vkCmdDraw(cmd, uint32_t(s.overlay.size()), 1, lineFirst[3], 0);
        }

        if (overlayTrisPipeline && !s.overlayTris.empty())
        {
            glm::vec2 invSize(1.0f / float(v.extent.width), 1.0f / float(v.extent.height));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayTrisPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayTrisLayout, 0, 1, &atlasSet, 0, nullptr);
            vkCmdPushConstants(cmd, overlayTrisLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::vec2), &invSize);
            VkDeviceSize offset = lineBytes;
            vkCmdBindVertexBuffers(cmd, 0, 1, &f.lines, &offset);
            vkCmdDraw(cmd, uint32_t(s.overlayTris.size()), 1, 0, 0);
        }

        vkCmdEndRendering(cmd);
        if (f.feedback.active())
            f.feedback.end(vk, cmd);
        timestamp(cmd, f, 3);
        v.timing.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
        v.timing.meshDrawCalls = frameMeshDraws;
        v.timing.meshInstances = frameMeshInstances;
        if (v.offscreen)
            return finishOffscreen(v, f, cmd);
        barrier(cmd, v.images[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, colorStage, colorWrite,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_ASPECT_COLOR_BIT);
        vkEndCommandBuffer(cmd);

        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = f.acquired;
        wait.stageMask = colorStage;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = v.rendered[imageIndex];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &wait;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &signal;
        vkQueueSubmit2(vk.queue, 1, &si, f.fence);

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &v.rendered[imageIndex];
        pi.swapchainCount = 1;
        pi.pSwapchains = &v.swapchain;
        pi.pImageIndices = &imageIndex;
        r = vkQueuePresentKHR(vk.queue, &pi);
        v.frameIndex = (v.frameIndex + 1) % kFramesInFlight;
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        {
            v.needsRecreate = true;
            return true;
        }
        return false;
    }

    /**
     * Ends the scene pass (resolved into postColor), then restarts rendering on the frame's own targets with the depth
     * kept, filtering postColor over every pixel; lines and overlays then draw on top unfiltered.
     */
    void recordPost(VkCommandBuffer cmd, Viewport& v, const FrameState& s, VkRenderingInfo ri, VkRenderingAttachmentInfo color,
                    VkRenderingAttachmentInfo depth)
    {
        LILA_ZONE();

        vkCmdEndRendering(cmd);
        constexpr auto colorStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier(cmd, v.postColor.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, colorStage,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT);
        constexpr auto depthStage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr auto depthAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        barrier(cmd, v.depth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depthStage, depthAccess,
                depthStage, depthAccess, VK_IMAGE_ASPECT_DEPTH_BIT);

        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport viewport{0, 0, float(v.extent.width), float(v.extent.height), 0, 1};
        VkRect2D scissor{{0, 0}, v.extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        PostPush p;
        p.size = glm::vec4(float(v.extent.width), float(v.extent.height), 1.0f / float(v.extent.width), 1.0f / float(v.extent.height));
        p.params = glm::vec4(s.kuwahara ? 1.0f : 0.0f, std::clamp(s.kuwaharaRadius, 1.0f, 12.0f), s.outline && !s.xray ? 1.0f : 0.0f,
                             std::clamp(s.outlineWidth, 0.5f, 8.0f));
        p.outline = glm::vec4(s.outlineColor, 1.0f);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout, 0, 1, &v.postSet, 0, nullptr);
        vkCmdPushConstants(cmd, postLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    /** Copies the offscreen target into the readback buffer and waits for the GPU. Never asks for a retry. */
    bool finishOffscreen(Viewport& v, Frame& f, VkCommandBuffer cmd)
    {
        barrier(cmd, v.images[0], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {v.extent.width, v.extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, v.images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, v.readback.buffer, 1, &region);
        vkEndCommandBuffer(cmd);

        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        vkQueueSubmit2(vk.queue, 1, &si, f.fence);
        vkWaitForFences(vk.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
        f.feedback.collect();
        readTimings(v, f);
        vmaInvalidateAllocation(vk.allocator, v.readback.alloc, 0, VK_WHOLE_SIZE);
        v.frameIndex = (v.frameIndex + 1) % kFramesInFlight;
        return false;
    }

    /** Render thread: one capture on a throwaway offscreen viewport. */
    void runOffscreen(OffscreenJob& job)
    {
        LILA_ZONE();

        Viewport v;
        v.offscreen = true;
        v.width = job.width;
        v.height = job.height;
        initFrames(v);
        v.needsRecreate = true;
        // Repeats reuse the targets, so their timings are steady-state frames like a live viewport's.
        std::vector<Renderer::FrameStats> runs;
        for (int i = 0; i < std::max(job.repeat, 1); ++i)
        {
            render(v, job.state);
            runs.push_back(v.timing);
        }
        std::nth_element(runs.begin(), runs.begin() + runs.size() / 2, runs.end(),
                         [](const Renderer::FrameStats& a, const Renderer::FrameStats& b) { return a.gpuMs < b.gpuMs; });
        job.timing = runs[runs.size() / 2];
        job.timing.frames = runs.size();
        if (v.readbackMapped)
        {
            const auto* src = static_cast<const uint8_t*>(v.readbackMapped);
            job.pixels.assign(src, src + size_t(v.extent.width) * v.extent.height * 4);
            // The pipelines target whatever the swapchain picked; callers always get BGRA.
            if (v.format == VK_FORMAT_R8G8B8A8_SRGB || v.format == VK_FORMAT_R8G8B8A8_UNORM)
                for (size_t i = 0; i + 3 < job.pixels.size(); i += 4)
                    std::swap(job.pixels[i], job.pixels[i + 2]);
            job.ok = true;
        }
        else
            job.error = "offscreen target could not be created";
        destroyViewport(v);
    }

    glm::vec4 envParams(const FrameState& s) const
    {
        float r = glm::radians(s.envRotation);
        return {std::cos(r), std::sin(r), s.envIntensity, envMaxLod};
    }

    /** One depth group of image planes, in frame order. In the scene group, blended picks the see-through ones. */
    void drawImagePlanes(VkCommandBuffer cmd, const FrameState& s, const glm::mat4& viewProj, ImagePlaneDraw::Depth depth, bool blended)
    {
        if (s.imagePlanes.empty())
            return;
        LILA_ZONE();

        bool scene = depth == ImagePlaneDraw::Depth::Scene;
        VkPipeline pipeline = !scene ? imageOverPipeline : blended ? imageBlendPipeline : imageDepthPipeline;
        if (!pipeline)
            return;
        bool bound = false;
        for (const ImagePlaneDraw& p : s.imagePlanes)
        {
            if (p.depth != depth || !p.image || (scene && (p.opacity < 0.999f) != blended) || textureKey(p.image, 0) == 0)
                continue;
            Material material;
            material.baseColorMap = p.image;
            VkDescriptorSet set = materialSet(&material);
            if (!set)
                continue;
            if (!bound)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                bound = true;
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, imagePlaneLayout, 0, 1, &set, 0, nullptr);
            ImagePlanePush push;
            push.viewProj = viewProj;
            for (int i = 0; i < 4; ++i)
                push.corners[i] = glm::vec4(p.corners[i], 1.0f);
            push.uv = p.uv;
            push.params = glm::vec4(std::clamp(p.opacity, 0.0f, 1.0f), p.brightness, p.contrast, p.grayscale ? 1.0f : 0.0f);
            push.texel = glm::vec4(0.5f / float(std::max(p.image->width, 1)), 0.5f / float(std::max(p.image->height, 1)), 0.0f, 0.0f);
            vkCmdPushConstants(cmd, imagePlaneLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
    }

    /** A soft blob under each shaded mesh that rests near the ground, fading out as it lifts off. */
    void drawContactShadows(VkCommandBuffer cmd, const FrameState& s, const glm::mat4& viewProj)
    {
        LILA_ZONE();

        int up = s.zUp ? 2 : 1;
        int a0 = 0, a1 = s.zUp ? 1 : 2;
        if (!contactPipeline || s.groundShadow <= 0.0f || s.eye[up] <= 0.0)
            return;
        bool bound = false;
        for (const MeshDraw& d : s.meshes)
        {
            if (!(d.flags & DrawShaded) || !d.mesh || d.mesh->indices.empty())
                continue;
            glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
            auto extend = [&](const glm::mat4& world) {
                for (int c = 0; c < 8; ++c)
                {
                    glm::vec3 corner((c & 1) ? d.mesh->boundsMax.x : d.mesh->boundsMin.x, (c & 2) ? d.mesh->boundsMax.y : d.mesh->boundsMin.y,
                                     (c & 4) ? d.mesh->boundsMax.z : d.mesh->boundsMin.z);
                    glm::vec3 w(world * glm::vec4(corner, 1.0f));
                    lo = glm::min(lo, w);
                    hi = glm::max(hi, w);
                }
            };
            if (d.instances && !d.instances->empty())
                for (const glm::mat4& placement : *d.instances)
                    extend(d.world * placement);
            else
                extend(d.world);
            glm::vec3 half = (hi - lo) * 0.5f;
            float size = std::max(half[a0], half[a1]);
            float gap = lo[up];
            if (size <= 0.0f || hi[up] <= 0.0f)
                continue;
            float opacity = s.groundShadow * (1.0f - std::clamp(gap / size, 0.0f, 1.0f));
            if (opacity <= 0.0f)
                continue;

            ContactPush p;
            p.viewProj = viewProj;
            glm::vec3 center = (lo + hi) * 0.5f;
            center[up] = 0.0f;
            p.center = glm::vec4(center, opacity);
            // The blob reaches past the footprint by a fraction of the object's size so it reads as soft occlusion.
            glm::vec3 axisA(0.0f), axisB(0.0f);
            float spread = size * 0.6f;
            axisA[a0] = half[a0] + spread;
            axisB[a1] = half[a1] + spread;
            p.axisA = glm::vec4(axisA, size / (size + spread));
            p.axisB = glm::vec4(axisB, 0.0f);
            if (!bound)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, contactPipeline);
                bound = true;
            }
            vkCmdPushConstants(cmd, contactLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
    }

    /**
     * kNoHandle when a map is missing or none of it uploaded, else something other than kNoHandle: its table handle on
     * the bindless path.
     */
    uint32_t mapHandle(const TexturePtr& t, uint64_t placeholder)
    {
        if (!t || textureKey(t, 0) == 0)
            return kNoHandle;
        if (!bindless)
            return 0;
        if (!frameTableAddress)
            return kNoHandle;
        auto it = frameHandles.find({t->key, placeholder});
        return it != frameHandles.end() ? it->second : kNoHandle;
    }

    /** A draw's material sections, or one section of slot 0 covering the whole mesh. */
    static std::vector<MeshSection> sectionsOf(const MeshDraw& d, const GpuMesh& g)
    {
        std::vector<MeshSection> sections = d.mesh->sections;
        if (sections.empty())
            sections.push_back({0, 0, g.indexCount});
        return sections;
    }

    /** One material section of a batch's placements, instanced, with whichever mesh pipeline is bound. */
    void drawSection(VkCommandBuffer cmd, const FrameState& s, const DrawBatch& b, GpuMesh& g, const MeshSection& sec, const glm::mat4& viewProj,
                     VkDescriptorSet& boundSet)
    {
        if (!frameInstanceAddress || b.count == 0)
            return;
        const MeshDraw& d = *b.draw;
        const Material* mat = sec.material < d.materials.size() ? &d.materials[sec.material] : nullptr;
        MeshPush p;
        p.viewProj = viewProj;
        p.instances = frameInstanceAddress;
        p.firstInstance = b.first;
        p.eye = glm::vec4(glm::vec3(s.eye), 1.0f);
        float lighting = s.lighting == FrameState::Lighting::Flat ? 2.0f
                         : s.lighting == FrameState::Lighting::Environment && envLoaded ? 1.0f
                                                                                        : 0.0f;
        p.params = glm::vec4(s.exposure, s.zUp ? 1.0f : 0.0f, 0.0f, lighting);
        p.env = envParams(s);
        p.gamma = s.gamma;
        if (mat)
        {
            p.base = glm::vec4(mat->baseColor, mat->roughness);
            p.surface = glm::vec4(mat->metallic, mat->specular, mat->opacity, mat->opacityClip);
            p.emissive = mat->emissive;
            p.flags = mat->flags;
            // Maps whose upload failed fall back to the constants.
            const uint32_t base = mapHandle(mat->baseColorMap, kWhiteSrgbKey), normal = mapHandle(mat->normalMap, kFlatNormalKey);
            const uint32_t orm = mapHandle(mat->ormMap, kWhiteLinearKey), emissive = mapHandle(mat->emissiveMap, kWhiteSrgbKey);
            if (base == kNoHandle)
                p.flags &= ~MatBaseColorMap;
            if (normal == kNoHandle)
                p.flags &= ~(MatNormalMap | MatNormalGreenUp);
            if (orm == kNoHandle)
                p.flags &= ~(MatMetallicMap | MatRoughnessMap | MatOcclusionMap);
            if (emissive == kNoHandle)
                p.flags &= ~MatEmissiveMap;
            p.maps[0] = base | normal << 16;
            p.maps[1] = orm | emissive << 16;
            p.textureTable = frameTableAddress;
            if (frameTableAddress)
                if (auto it = frameStyles.find({&d, sec.material}); it != frameStyles.end())
                    p.style = it->second;
        }
        else
        {
            p.base = glm::vec4(d.baseColor, d.roughness);
            p.surface = glm::vec4(0.0f, 0.5f, 1.0f, 0.3333f);
            p.flags = MatTwoSided;
        }
        if (d.vertexColors != VertexColorMode::Off)
            p.flags |= (d.vertexColors == VertexColorMode::Unlit ? kVertexColorUnlit : kVertexColorShaded) |
                       (uint32_t(d.colorChannel) << kColorChannelShift);
        if (!s.twoSidedLighting)
            p.flags |= kOneSidedLighting;
        if (s.backfaceTint.a > 0.0f)
            p.backTint = glm::packUnorm4x8(glm::clamp(s.backfaceTint, 0.0f, 1.0f));
        if (s.xray)
        {
            p.flags |= MatTranslucent;
            p.surface.z *= kXrayOpacity;
            p.surface.w *= kXrayOpacity; // masked cutouts stay where they were
        }
        VkDescriptorSet set = bindless ? bindlessSet : materialSet(mat);
        if (!set)
            return;
        if (set != boundSet)
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &set, 0, nullptr);
            boundSet = set;
        }
        // The viewport toggle alone decides, as Maya's does: one-sided materials show their back faces with it off.
        vkCmdSetCullMode(cmd, s.backfaceCulling ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
        vkCmdPushConstants(cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g.vertices.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, g.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        uint32_t first = std::min(sec.firstIndex, g.indexCount);
        if (b.wire)
        {
            p.edgeMask = g.edgeMask.address;
            p.firstTriangle = first / 3;
            p.wireWidth = std::max(d.wireWidth, 1.0f);
            vkCmdPushConstants(cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
        }
        vkCmdDrawIndexed(cmd, std::min(sec.indexCount, g.indexCount - first), b.count, first, 0, 0);
        ++frameMeshDraws;
        frameMeshInstances += b.count;
    }

    void drawMeshes(VkCommandBuffer cmd, const FrameState& s, const glm::mat4& viewProj, VkDescriptorSet aoSet, uint32_t heightPx)
    {
        LILA_ZONE();

        if (s.meshes.empty() || !meshPipeline)
            return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &envSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 2, 1, &aoSet, 0, nullptr);

        struct Pending
        {
            DrawBatch placement; // one instance of a batch: translucent placements sort individually
            MeshSection section;
            float distance;
        };
        std::vector<Pending> translucent;
        VkDescriptorSet boundSet = VK_NULL_HANDLE;
        VkPipeline bound = meshPipeline;
        auto use = [&](VkPipeline p) {
            if (p && p != bound)
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
        };

        for (const DrawBatch& b : frameBatches.shaded)
        {
            const MeshDraw& d = *b.draw;
            GpuMesh* g = gpuMesh(d.mesh);
            use(b.wire && g->edgeMask.address && meshWirePipeline ? meshWirePipeline : meshPipeline);
            for (const MeshSection& sec : sectionsOf(d, *g))
            {
                const Material* mat = sec.material < d.materials.size() ? &d.materials[sec.material] : nullptr;
                if (s.xray || (mat && (mat->flags & MatTranslucent)))
                {
                    const glm::vec4 centre((d.mesh->boundsMin + d.mesh->boundsMax) * 0.5f, 1.0f);
                    for (uint32_t i = 0; i < b.count; ++i)
                    {
                        const glm::vec3 c(frameBatches.instances[b.first + i].world * centre);
                        translucent.push_back({{b.draw, b.first + i, 1, b.wire}, sec, glm::length(c - glm::vec3(s.eye))});
                    }
                    continue;
                }
                drawSection(cmd, s, b, *g, sec, viewProj, boundSet);
            }
        }
        // Translucency after everything opaque, far to near, testing depth without writing it.
        if (!translucent.empty() && meshBlendPipeline)
        {
            std::ranges::stable_sort(translucent, [](const Pending& a, const Pending& b) { return a.distance > b.distance; });
            for (const Pending& t : translucent)
            {
                GpuMesh* g = gpuMesh(t.placement.draw->mesh);
                use(t.placement.wire && g->edgeMask.address && meshWireBlendPipeline ? meshWireBlendPipeline : meshBlendPipeline);
                drawSection(cmd, s, t.placement, *g, t.section, viewProj, boundSet);
            }
        }
        evictTextures();

        if (!wirePipeline || !frameInstanceAddress)
            return;
        for (const DrawBatch& b : frameBatches.wire)
        {
            const MeshDraw& d = *b.draw;
            GpuMesh* g = gpuMesh(d.mesh);
            WirePush p;
            p.viewProj = viewProj;
            p.instances = frameInstanceAddress;
            p.firstInstance = b.first;
            bool shaded = d.flags & (DrawShaded | DrawWireDepth);
            if (shaded)
                p.depthPull = depthPull(s.proj, heightPx, kLinePullPixels + 0.5 * lineWidth(d.wireWidth));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shaded ? wirePipeline : wireXrayPipeline);
            vkCmdSetLineWidth(cmd, lineWidth(d.wireWidth));
            vkCmdPushConstants(cmd, wireLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &g->edges.buffer, &offset);
            if (g->edgeIndexCount)
            {
                vkCmdBindIndexBuffer(cmd, g->edgeIndices.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, g->edgeIndexCount, b.count, 0, 0, 0);
            }
            else
                vkCmdDraw(cmd, g->edgePointCount, b.count, 0, 0);
        }
        evictMeshes();
    }

    void drawPoints(VkCommandBuffer cmd, const FrameState& s, const glm::mat4& viewProj, uint32_t heightPx)
    {
        if (!pointsPipeline || s.points.empty())
            return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pointsPipeline);
        for (const PointDraw& d : s.points)
        {
            auto it = d.cloud ? pointCache.find(d.cloud->key) : pointCache.end();
            if (it == pointCache.end())
                continue;
            it->second.lastUsed = frameCounter;
            PointsPush p;
            p.viewProj = viewProj;
            p.depthPull = depthPull(s.proj, heightPx, kLinePullPixels + 0.5 * std::max(d.size, d.selectedSize));
            for (int i = 0; i < 3; ++i)
                p.colors[i] = d.colors[i];
            const float* range = vk.properties.limits.pointSizeRange;
            p.sizes = glm::vec4(std::clamp(d.size, range[0], range[1]), std::clamp(d.selectedSize, range[0], range[1]), 0.0f, 0.0f);
            vkCmdPushConstants(cmd, pointsLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(p), &p);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &it->second.buffer.buffer, &offset);
            vkCmdDraw(cmd, it->second.count, 1, 0, 0);
        }
        evictPoints();
    }

    /** One slice of a task; resets task when it finished or threw. */
    void runTaskSlice(std::unique_ptr<QueuedTask>& task)
    {
        LILA_ZONE();

        try
        {
            if (!task->step(vk))
                return;
            task->done.set_value();
        }
        catch (...)
        {
            task->done.set_exception(std::current_exception());
        }
        task.reset();
    }

    void loop()
    {
        LILA_THREAD_NAME("Render");
        std::unique_lock lock(mutex);
        auto ready = [&] {
            if (quit || !toRemove.empty())
                return true;
            // Captures wait out an environment build in flight, so they are lit like the views.
            if (!offscreenJobs.empty() && !envBusy)
                return true;
            if (!tasks.empty())
                return true;
            // A viewport with no frame state yet can't draw, and waking for it would spin with the mutex held.
            for (auto& [id, v] : viewports)
                if (v->pending && v->hasState)
                    return true;
            return false;
        };
        while (true)
        {
            // Big uploads leave big staging buffers, slow to free: they go once the GPU is done with them, while the
            // thread is idle, instead of in front of the next frame drawn into the same slot.
            if (reclaimStaging())
                cv.wait_for(lock, std::chrono::milliseconds(20), ready);
            else
                cv.wait(lock, ready);
            if (quit)
                break;

            for (ViewportId id : toRemove)
                if (auto it = viewports.find(id); it != viewports.end())
                {
                    destroyViewport(*it->second);
                    viewports.erase(it);
                }
            if (!toRemove.empty())
            {
                toRemove.clear();
                removed.notify_all();
            }

            while (!offscreenJobs.empty() && !envBusy)
            {
                std::shared_ptr<OffscreenJob> job = offscreenJobs.front();
                std::unique_ptr<EnvironmentData> env = pollEnvironment(job->state.envPath);
                if (!env && envBusy)
                    break;
                offscreenJobs.pop_front();
                lock.unlock();
                if (env)
                    uploadEnvironment(*env);
                runOffscreen(*job);
                lock.lock();
                job->done = true;
                offscreenDone.notify_all();
            }

            if (!tasks.empty())
            {
                std::unique_ptr<QueuedTask> task = std::move(tasks.front());
                tasks.pop_front();
                lock.unlock();
                runTaskSlice(task);
                lock.lock();
                if (task)
                    tasks.push_back(std::move(task));
            }

            for (auto& [id, vp] : viewports)
            {
                Viewport& v = *vp;
                if (!v.pending || !v.hasState)
                    continue;
                v.pending = false;
                if (!v.surface)
                {
                    v.surface = createSurface(vk.instance, v.window);
                    initFrames(v);
                }
                if (!v.surface)
                    continue;
                std::unique_ptr<EnvironmentData> env = pollEnvironment(v.state.envPath);
                // Until the first environment arrives there is nothing to light with; the worker re-queues every viewport.
                if (!env && !envLoaded)
                    continue;
                FrameState s = v.state;
                lock.unlock();
                if (env)
                    uploadEnvironment(*env);
                auto latch = [&](FrameState& latest) {
                    std::scoped_lock relock(mutex);
                    // A new environment or sample count needs the setup that ran before the wait, so it waits a frame.
                    if (v.pending && v.state.envPath == latest.envPath && v.state.msaaSamples == latest.msaaSamples)
                    {
                        latest = v.state;
                        v.pending = false;
                    }
                };
                bool retry = render(v, s, latch);
                LILA_FRAME_MARK();
                lock.lock();
                ++v.timing.frames;
                v.stats = v.timing;
                if (retry && v.retries++ < 3)
                    v.pending = true;
            }
        }

        for (auto& t : tasks)
            t->done.set_exception(std::make_exception_ptr(std::runtime_error("the renderer stopped")));
        tasks.clear();
        for (auto& [id, v] : viewports)
            destroyViewport(*v);
        viewports.clear();
        // The worker takes the mutex when it finishes.
        lock.unlock();
        if (envWorker.joinable())
            envWorker.join();
        lock.lock();
        vkDeviceWaitIdle(vk.device);
        destroyMeshes();
        removed.notify_all();
    }
};

Renderer::Renderer() : m(std::make_unique<Impl>()) {}
Renderer::~Renderer() { stop(); }

Renderer& Renderer::get()
{
    static Renderer r;
    return r;
}

bool Renderer::start()
{
    if (m->started)
        return true;
#ifdef NDEBUG
    bool validation = std::getenv("LILA_VK_VALIDATION") != nullptr;
#else
    bool validation = true;
#endif
    if (!m->vk.init(validation))
        return false;
    if (m->vk.properties.limits.maxPushConstantsSize < 256)
        log::warn("render: GPU push constant limit is {} bytes; grid may not draw", m->vk.properties.limits.maxPushConstantsSize);
    m->initDescriptors();
    m->quit = false;
    m->started = true;
    m->thread = std::thread([this] { m->loop(); });
    return true;
}

void Renderer::stop()
{
    if (!m->started)
        return;
    {
        std::scoped_lock lock(m->mutex);
        m->quit = true;
    }
    m->cv.notify_all();
    m->thread.join();
    for (const auto& hook : m->stopHooks)
        hook(m->vk);
    compute::sharedPipelines().clear(m->vk);
    GpuImage::releaseAll(m->vk);
    m->destroyPipelines();
    m->destroyDescriptors();
    m->vk.shutdown();
    m->started = false;
}

void Renderer::addStopHook(std::function<void(VulkanContext& vk)> fn)
{
    m->stopHooks.push_back(std::move(fn));
}

bool Renderer::running() const { return m->started; }

ViewportId Renderer::addViewport(uintptr_t nativeWindow, int width, int height)
{
    std::scoped_lock lock(m->mutex);
    auto v = std::make_unique<Viewport>();
    v->window = nativeWindow;
    v->width = width;
    v->height = height;
    ViewportId id = m->nextId++;
    m->viewports[id] = std::move(v);
    return id;
}

void Renderer::removeViewport(ViewportId id)
{
    if (!m->started)
        return;
    std::unique_lock lock(m->mutex);
    m->toRemove.push_back(id);
    m->cv.notify_all();
    m->removed.wait(lock, [&] { return !m->viewports.contains(id) || m->quit; });
}

void Renderer::resize(ViewportId id, int width, int height)
{
    {
        std::scoped_lock lock(m->mutex);
        auto it = m->viewports.find(id);
        if (it == m->viewports.end())
            return;
        Viewport& v = *it->second;
        if (v.width == width && v.height == height)
            return;
        // The swapchain is rebuilt with the next submitted frame, which carries the matching projection.
        v.width = width;
        v.height = height;
        v.needsRecreate = true;
    }
}

void Renderer::submit(ViewportId id, FrameState state)
{
    {
        std::scoped_lock lock(m->mutex);
        auto it = m->viewports.find(id);
        if (it == m->viewports.end())
            return;
        it->second->state = std::move(state);
        it->second->pending = true;
        it->second->hasState = true;
        it->second->retries = 0;
    }
    m->cv.notify_all();
}

Renderer::FrameStats Renderer::stats(ViewportId id) const
{
    std::scoped_lock lock(m->mutex);
    auto it = m->viewports.find(id);
    return it == m->viewports.end() ? FrameStats{} : it->second->stats;
}

bool Renderer::renderOffscreen(FrameState state, int width, int height, std::vector<uint8_t>& bgra, std::string* err, FrameStats* timing,
                               int repeat)
{
    auto fail = [&](const std::string& e) {
        if (err)
            *err = e;
        return false;
    };
    if (!m->started)
        return fail("renderer not running");
    if (width < 1 || height < 1 || width > 8192 || height > 8192)
        return fail("size out of range");
    auto job = std::make_shared<OffscreenJob>();
    job->state = std::move(state);
    job->width = width;
    job->height = height;
    job->repeat = std::clamp(repeat, 1, 1000);
    std::unique_lock lock(m->mutex);
    m->offscreenJobs.push_back(job);
    m->cv.notify_all();
    if (!m->offscreenDone.wait_for(lock, std::chrono::seconds(60), [&] { return job->done || m->quit; }) || !job->done)
    {
        std::erase(m->offscreenJobs, job);
        return fail("timed out waiting for the render thread");
    }
    if (!job->ok)
        return fail(job->error);
    bgra = std::move(job->pixels);
    if (timing)
        *timing = job->timing;
    return true;
}

std::string Renderer::deviceName() const
{
    return m->vk.deviceName;
}

Renderer::GpuInfo Renderer::gpuInfo() const
{
    GpuInfo info;
    if (!running())
        return info;
    info.name = m->vk.deviceName;
    info.discrete = m->vk.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(m->vk.physical, &mem);
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i)
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            info.vramBytes += mem.memoryHeaps[i].size;
    return info;
}

std::future<void> Renderer::runTask(GpuTask task)
{
    auto t = std::make_unique<Impl::QueuedTask>();
    t->step = std::move(task);
    std::future<void> f = t->done.get_future();
    std::scoped_lock lock(m->mutex);
    if (!m->started || m->quit)
    {
        t->done.set_exception(std::make_exception_ptr(std::runtime_error("the renderer is not running")));
        return f;
    }
    m->tasks.push_back(std::move(t));
    m->cv.notify_all();
    return f;
}

VulkanContext* Renderer::context()
{
    return m->started ? &m->vk : nullptr;
}

std::shared_ptr<const EnvProbe> Renderer::environmentProbe(const std::string& path) const
{
    std::scoped_lock lock(m->probeMutex);
    return m->probePath == path ? m->probe : nullptr;
}
} // namespace lila::render
