#include "render/Renderer.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "render/Backdrop.h"
#include "render/Environment.h"
#include "render/HdrImage.h"
#include "render/DisplayOptions.h"
#include "render/Surface.h"
#include "render/VulkanContext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
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
};

struct MeshPush
{
    glm::mat4 viewProj;
    glm::mat4 world;
    glm::vec4 eye;
    glm::vec4 base;
    glm::vec4 params;
    glm::vec4 env;
    glm::vec4 surface;
    glm::vec3 emissive{0.0f};
    uint32_t flags = 0;
    glm::vec4 display{1.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(MeshPush) == 240, "matches MeshPush in mesh.slang");

// Draw-level bits above the material flags, matching mesh.slang.
constexpr uint32_t kVertexColorShaded = 1u << 11;
constexpr uint32_t kVertexColorUnlit = 1u << 12;
constexpr uint32_t kColorChannelShift = 13;

// Placeholder textures bound where a material has no map; never evicted.
constexpr uint64_t kWhiteSrgbKey = ~uint64_t(0);
constexpr uint64_t kWhiteLinearKey = ~uint64_t(0) - 1;
constexpr uint64_t kFlatNormalKey = ~uint64_t(0) - 2;
constexpr uint32_t kMaxMaterialSets = 1024;

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
};

constexpr VkFormat kAoNormalFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kAoFormat = VK_FORMAT_R32_SFLOAT;
constexpr uint32_t kMaxAoSets = 64;

struct WirePush
{
    glm::mat4 viewProj;
    glm::mat4 world;
    glm::vec4 color;
};

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
    Buffer vertices, indices, edges;
    uint32_t indexCount = 0;
    uint32_t edgePointCount = 0;
    std::shared_ptr<const RenderMesh> keep;
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
    auto path = paths::executableDir() / "shaders" / name;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        log::error("render: missing shader {}", path.string());
        return {};
    }
    size_t size = size_t(in.tellg());
    std::vector<uint32_t> code(size / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(code.data()), std::streamsize(size));
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
    ViewportId nextId = 1;

    VkPipelineLayout gridLayout = VK_NULL_HANDLE, linesLayout = VK_NULL_HANDLE, overlayTrisLayout = VK_NULL_HANDLE;
    VkPipeline gridPipeline = VK_NULL_HANDLE, linesPipeline = VK_NULL_HANDLE, overlayPipeline = VK_NULL_HANDLE;
    VkPipeline overlayTrisPipeline = VK_NULL_HANDLE;
    VkPipelineLayout meshLayout = VK_NULL_HANDLE, wireLayout = VK_NULL_HANDLE;
    VkPipeline meshPipeline = VK_NULL_HANDLE, wirePipeline = VK_NULL_HANDLE, wireXrayPipeline = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, GpuMesh> meshCache; // render thread only
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
    VkPipelineLayout contactLayout = VK_NULL_HANDLE;
    VkPipeline contactPipeline = VK_NULL_HANDLE;

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
        Image image;
        uint64_t lastUsed = 0;
    };
    struct MaterialSet
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t lastUsed = 0;
    };
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::map<std::array<uint64_t, 4>, MaterialSet> materialSets; // base color, normal, ORM, emissive texture keys
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool materialPool = VK_NULL_HANDLE;
    VkSampler materialSampler = VK_NULL_HANDLE;
    bool blitSrgb = false, blitLinear = false;
    bool warnedPool = false;
    VkPipeline meshBlendPipeline = VK_NULL_HANDLE;
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
        for (Image* img : {&v.aoDepth, &v.aoNormal, &v.aoRaw, &v.aoFinal})
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
            v.color = createImage(v.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, extent, vk.msaa,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
        v.msaaGeneration = msaaGeneration;
        v.depth = createImage(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, extent,
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
            v.color = createImage(v.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, extent, vk.msaa,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
        v.msaaGeneration = msaaGeneration;
        v.depth = createImage(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, extent,
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
    }

    void destroyViewport(Viewport& v)
    {
        vkDeviceWaitIdle(vk.device);
        for (Frame& f : v.frames)
        {
            freeStaging(f);
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
        for (VkDescriptorSet set : {v.gtaoSet, v.denoiseSet, v.aoSampleSet})
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
    Image createTexture(VkFormat format, VkExtent2D extent, uint32_t mips, bool cube, VkImageUsageFlags extraUsage = 0)
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
        if (size)
            std::memcpy(info.pMappedData, data, size);
        vmaFlushAllocation(vk.allocator, b.alloc, 0, VK_WHOLE_SIZE);
        return b;
    }

    void destroyBuffer(Buffer& b)
    {
        if (b.buffer)
            vmaDestroyBuffer(vk.allocator, b.buffer, b.alloc);
        b = {};
    }

    GpuMesh* gpuMesh(const std::shared_ptr<const RenderMesh>& mesh)
    {
        LILA_ZONE();

        auto it = meshCache.find(mesh->key);
        if (it == meshCache.end())
        {
            GpuMesh g;
            if (mesh->dynamic)
            {
                DynamicVertices& dv = *mesh->dynamic;
                std::scoped_lock lock(dv.mutex);
                g.vertices = createBuffer(dv.vertices.data(), dv.vertices.size() * sizeof(MeshVertex),
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
                dv.dirty.clear();
                dv.allDirty = false;
            }
            else
                g.vertices = createBuffer(mesh->vertices.data(), mesh->vertices.size() * sizeof(MeshVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            g.indices = createBuffer(mesh->indices.data(), mesh->indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            g.edges = createBuffer(mesh->edgePoints.data(), mesh->edgePoints.size() * sizeof(glm::vec3), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            g.indexCount = uint32_t(mesh->indices.size());
            g.edgePointCount = uint32_t(mesh->edgePoints.size());
            g.keep = mesh;
            it = meshCache.emplace(mesh->key, std::move(g)).first;
        }
        it->second.lastUsed = frameCounter;
        return &it->second;
    }

    /** Drops GPU copies no frame has drawn for a while. */
    void evictMeshes()
    {
        if (frameCounter % 120 != 0)
            return;
        std::vector<uint64_t> stale;
        for (auto& [key, g] : meshCache)
            if (g.lastUsed + 240 < frameCounter)
                stale.push_back(key);
        if (stale.empty())
            return;
        vkDeviceWaitIdle(vk.device);
        for (uint64_t key : stale)
        {
            GpuMesh& g = meshCache[key];
            destroyBuffer(g.vertices);
            destroyBuffer(g.indices);
            destroyBuffer(g.edges);
            meshCache.erase(key);
        }
    }

    void destroyMeshes()
    {
        for (auto& [key, g] : meshCache)
        {
            destroyBuffer(g.vertices);
            destroyBuffer(g.indices);
            destroyBuffer(g.edges);
        }
        meshCache.clear();
    }

    // ---------------------------------------------------------------- materials

    void initMaterials()
    {
        LILA_ZONE();

        VkDescriptorSetLayoutBinding b[4];
        for (uint32_t i = 0; i < 4; ++i)
            b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 4;
        lci.pBindings = b;
        vkCreateDescriptorSetLayout(vk.device, &lci, nullptr, &materialSetLayout);

        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxMaterialSets * 4};
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

        auto canBlit = [&](VkFormat f) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(vk.physical, f, &fp);
            VkFormatFeatureFlags need =
                VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            return (fp.optimalTilingFeatures & need) == need;
        };
        blitSrgb = canBlit(VK_FORMAT_R8G8B8A8_SRGB);
        blitLinear = canBlit(VK_FORMAT_R8G8B8A8_UNORM);

        auto placeholder = [&](uint64_t key, VkFormat format, std::array<uint8_t, 4> texel) {
            GpuTexture& t = textures[key];
            t.image = createTexture(format, {1, 1}, 1, false);
            uploadImage(t.image.image, {1, 1}, 1, 1, 4, {texel.data()});
        };
        placeholder(kWhiteSrgbKey, VK_FORMAT_R8G8B8A8_SRGB, {255, 255, 255, 255});
        placeholder(kWhiteLinearKey, VK_FORMAT_R8G8B8A8_UNORM, {255, 255, 255, 255});
        placeholder(kFlatNormalKey, VK_FORMAT_R8G8B8A8_UNORM, {128, 128, 255, 255});
    }

    void destroyMaterials()
    {
        for (auto& [key, t] : textures)
            destroyImage(t.image);
        textures.clear();
        materialSets.clear();
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

        if (t.width <= 0 || t.height <= 0 || t.rgba.size() < size_t(t.width) * size_t(t.height) * 4)
            return false;
        VkFormat format = t.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        VkExtent2D extent{uint32_t(t.width), uint32_t(t.height)};
        uint32_t mips = 1;
        if (t.srgb ? blitSrgb : blitLinear)
            while ((std::max(extent.width, extent.height) >> mips) > 0)
                ++mips;

        GpuTexture g;
        g.image = createTexture(format, extent, mips, false, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        g.lastUsed = frameCounter;
        Buffer staging = createBuffer(t.rgba.data(), size_t(t.width) * size_t(t.height) * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
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
        textures[t.key] = g;
        return true;
    }

    /** Key of an uploaded texture, or the placeholder's when the map is missing or failed to upload. */
    uint64_t textureKey(const TexturePtr& t, uint64_t placeholder)
    {
        if (!t)
            return placeholder;
        auto it = textures.find(t->key);
        if (it == textures.end())
            return placeholder;
        it->second.lastUsed = frameCounter;
        return t->key;
    }

    /** Uploads every map this frame's materials use for the first time. Runs before rendering begins. */
    void prepareTextures(VkCommandBuffer cmd, Frame& f, const FrameState& s)
    {
        LILA_ZONE();

        for (const MeshDraw& d : s.meshes)
        {
            if (!(d.flags & DrawShaded))
                continue;
            for (const Material& mat : d.materials)
                for (const TexturePtr* t : {&mat.baseColorMap, &mat.normalMap, &mat.ormMap, &mat.emissiveMap})
                    if (*t && !textures.contains((*t)->key) && !uploadTexture(cmd, f, **t))
                        log::warn("render: skipped a malformed {}x{} texture", (*t)->width, (*t)->height);
        }
        for (const ImagePlaneDraw& p : s.imagePlanes)
            if (p.image && !textures.contains(p.image->key) && !uploadTexture(cmd, f, *p.image))
                log::warn("render: skipped a malformed {}x{} image plane", p.image->width, p.image->height);
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
            if (regions.empty())
                continue;
            if (first)
            {
                // Earlier frames may still read these vertices.
                memoryBarrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                first = false;
            }
            Buffer staging = createBuffer(data.data(), data.size() * sizeof(MeshVertex), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            f.staging.push_back(staging);
            vkCmdCopyBuffer(cmd, staging.buffer, it->second.vertices.buffer, uint32_t(regions.size()), regions.data());
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
        std::array<uint64_t, 4> keys = {kWhiteSrgbKey, kFlatNormalKey, kWhiteLinearKey, kWhiteSrgbKey};
        if (m)
            keys = {textureKey(m->baseColorMap, kWhiteSrgbKey), textureKey(m->normalMap, kFlatNormalKey), textureKey(m->ormMap, kWhiteLinearKey),
                    textureKey(m->emissiveMap, kWhiteSrgbKey)};
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
                std::array<uint64_t, 4> fallback = {kWhiteSrgbKey, kFlatNormalKey, kWhiteLinearKey, kWhiteSrgbKey};
                auto fb = materialSets.find(fallback);
                return fb != materialSets.end() ? fb->second.set : VK_NULL_HANDLE;
            }
            VkDescriptorImageInfo info[4];
            VkWriteDescriptorSet w[4]{};
            for (uint32_t i = 0; i < 4; ++i)
            {
                info[i] = {materialSampler, textures[keys[i]].image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = set;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = &info[i];
            }
            vkUpdateDescriptorSets(vk.device, 4, w, 0, nullptr);
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
        std::vector<std::array<uint64_t, 4>> deadSets;
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
            destroyImage(textures[key].image);
            textures.erase(key);
        }
    }

    void freeStaging(Frame& f)
    {
        for (Buffer& b : f.staging)
            destroyBuffer(b);
        f.staging.clear();
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

        VkDescriptorSetLayoutBinding sb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 1;
        lci.pBindings = &sb;
        vkCreateDescriptorSetLayout(vk.device, &lci, nullptr, &aoSampleLayout);

        VkDescriptorSetLayoutBinding cb[4];
        for (uint32_t i = 0; i < 4; ++i)
            cb[i] = {i, i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        lci.bindingCount = 4;
        lci.pBindings = cb;
        vkCreateDescriptorSetLayout(vk.device, &lci, nullptr, &aoComputeLayout);

        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxAoSets * 3}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxAoSets}};
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
        writeImage(whiteAoSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, whiteAo.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
    }

    static bool anyShaded(const FrameState& s)
    {
        return std::ranges::any_of(s.meshes, [](const MeshDraw& d) { return (d.flags & DrawShaded) && d.mesh && !d.mesh->indices.empty(); });
    }

    /**
     * Records the depth/normal prepass and both AO dispatches ahead of the main pass, leaving v.aoFinal ready for the
     * mesh shader. The images are shared by both frames in flight, so each first use waits on the previous frame's reads.
     */
    void recordAo(VkCommandBuffer cmd, Viewport& v, const FrameState& s, const glm::mat4& viewProj)
    {
        LILA_ZONE();

        constexpr auto compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        constexpr auto fragment = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        constexpr auto colorStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        constexpr auto depthStage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        constexpr auto depthWrite = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        constexpr auto read = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        constexpr auto storageWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        constexpr auto general = VK_IMAGE_LAYOUT_GENERAL;
        constexpr auto color = VK_IMAGE_ASPECT_COLOR_BIT;

        barrier(cmd, v.aoDepth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, compute, 0, depthStage, depthWrite,
                VK_IMAGE_ASPECT_DEPTH_BIT);
        barrier(cmd, v.aoNormal.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, compute, 0, colorStage,
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
        for (const MeshDraw& d : s.meshes)
        {
            if (!(d.flags & DrawShaded) || !d.mesh || d.mesh->indices.empty())
                continue;
            GpuMesh* g = gpuMesh(d.mesh);
            std::vector<MeshSection> sections = d.mesh->sections;
            if (sections.empty())
                sections.push_back({0, 0, g->indexCount});
            for (const MeshSection& sec : sections)
            {
                const Material* mat = sec.material < d.materials.size() ? &d.materials[sec.material] : nullptr;
                if (!mat || !(mat->flags & MatTranslucent))
                    drawSection(cmd, s, d, *g, sec, viewProj, boundSet);
            }
        }
        vkCmdEndRendering(cmd);

        barrier(cmd, v.aoDepth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, depthStage,
                depthWrite, compute, read, VK_IMAGE_ASPECT_DEPTH_BIT);
        barrier(cmd, v.aoNormal.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, colorStage,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, compute, read, color);
        barrier(cmd, v.aoRaw.image, VK_IMAGE_LAYOUT_UNDEFINED, general, compute, 0, compute, storageWrite, color);

        glm::dmat4 proj = s.proj;
        AoPush p;
        p.invProj = glm::mat4(glm::inverse(proj));
        p.view = glm::mat4(s.view);
        bool ortho = proj[3][3] == 1.0;
        p.params = glm::vec4(std::max(s.aoRadius, 0.01f), std::max(s.aoIntensity, 0.0f), float(proj[0][0] * 0.5 * v.extent.width),
                             ortho ? 1.0f : 0.0f);
        p.size = glm::vec4(float(v.extent.width), float(v.extent.height), 1.0f / float(v.extent.width), 1.0f / float(v.extent.height));
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

    VkShaderModule module(const std::vector<uint32_t>& code)
    {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(vk.device, &ci, nullptr, &mod);
        return mod;
    }

    VkPipeline buildPipeline(VkPipelineLayout layout, VkShaderModule mod, const char* vs, const char* ps, VkFormat format,
                             VkPrimitiveTopology topology, const VkPipelineVertexInputStateCreateInfo& vertexInput, bool depthWrite,
                             bool blend, bool depthTest = true, float depthBias = 0.0f, bool dynamicCull = false, bool singleSample = false)
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
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = dynamicCull ? 3 : 2;
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
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, entry};
        ci.layout = layout;
        VkPipeline p = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS)
            log::error("render: compute pipeline creation failed ({})", entry);
        return p;
    }

    void createPipelines(VkFormat format)
    {
        LILA_ZONE();

        destroyPipelines();
        pipelineFormat = format;

        auto layoutFor = [&](uint32_t size, VkShaderStageFlags stages, VkDescriptorSetLayout set = VK_NULL_HANDLE) {
            VkPushConstantRange range{stages, 0, size};
            VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            lci.pushConstantRangeCount = 1;
            lci.pPushConstantRanges = &range;
            lci.setLayoutCount = set ? 1 : 0;
            lci.pSetLayouts = &set;
            VkPipelineLayout l;
            vkCreatePipelineLayout(vk.device, &lci, nullptr, &l);
            return l;
        };
        gridLayout = layoutFor(sizeof(GridPush), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        linesLayout = layoutFor(sizeof(glm::mat4), VK_SHADER_STAGE_VERTEX_BIT);
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
            linesOnTopPipeline =
                buildPipeline(linesLayout, lines, "linesVS", "linesPS", format, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, vin, false, true, true, 2.0f);
            tris3dPipeline =
                buildPipeline(linesLayout, lines, "linesVS", "linesPS", format, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, vin, false, true, true, 2.0f);
        }

        {
            VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPush)};
            VkDescriptorSetLayout sets[3] = {envSetLayout, materialSetLayout, aoSampleLayout};
            VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            lci.pushConstantRangeCount = 1;
            lci.pPushConstantRanges = &range;
            lci.setLayoutCount = 3;
            lci.pSetLayouts = sets;
            vkCreatePipelineLayout(vk.device, &lci, nullptr, &meshLayout);
        }
        wireLayout = layoutFor(sizeof(WirePush), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        if (VkShaderModule mesh = module(loadSpirv("mesh.spv")))
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
            wirePipeline = buildPipeline(wireLayout, wire, "wireVS", "wirePS", format, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, wvin, false, true, true, 2.0f);
            wireXrayPipeline = buildPipeline(wireLayout, wire, "wireVS", "wirePS", format, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, wvin, false, true, false);
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
        for (VkPipeline* p : {&gridPipeline, &linesPipeline, &overlayPipeline, &overlayTrisPipeline, &meshPipeline, &meshBlendPipeline, &wirePipeline,
                              &wireXrayPipeline, &backgroundPipeline, &linesOnTopPipeline, &tris3dPipeline, &contactPipeline, &prepassPipeline, &gtaoPipeline,
                              &denoisePipeline, &imageOverPipeline, &imageDepthPipeline, &imageBlendPipeline})
            if (*p)
            {
                vkDestroyPipeline(vk.device, *p, nullptr);
                *p = VK_NULL_HANDLE;
            }
        for (VkPipelineLayout* l : {&gridLayout, &linesLayout, &overlayTrisLayout, &meshLayout, &wireLayout, &contactLayout, &aoLayout,
                                    &imagePlaneLayout})
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
        freeStaging(f);
        readTimings(v, f);

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

        // One buffer per frame: lines, linesOnTop, tris3d and overlay share the LineVertex stride, overlayTris follow.
        const std::vector<LineVertex>* lineSets[] = {&s.lines, &s.linesOnTop, &s.tris3d, &s.overlay};
        uint32_t lineFirst[4];
        size_t lineBytes = 0;
        for (int i = 0; i < 4; ++i)
        {
            lineFirst[i] = uint32_t(lineBytes / sizeof(LineVertex));
            lineBytes += lineSets[i]->size() * sizeof(LineVertex);
        }
        size_t trisBytes = s.overlayTris.size() * sizeof(OverlayVertex);
        if (lineBytes + trisBytes)
        {
            ensureLines(f, lineBytes + trisBytes);
            for (int i = 0; i < 4; ++i)
                std::memcpy(static_cast<char*>(f.linesMapped) + size_t(lineFirst[i]) * sizeof(LineVertex), lineSets[i]->data(),
                            lineSets[i]->size() * sizeof(LineVertex));
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
        prepareDynamicMeshes(cmd, f, s);

        glm::dmat4 viewProj = s.proj * s.view;
        glm::mat4 vpF(viewProj);
        bool ao = s.ambientOcclusion && s.aoIntensity > 0.0f && prepassPipeline && gtaoPipeline && denoisePipeline && v.gtaoSet && v.denoiseSet &&
                  v.aoSampleSet && anyShaded(s) && !s.xray;
        f.aoTimed = ao;
        timestamp(cmd, f, 1);
        const auto aoStart = std::chrono::steady_clock::now();
        if (ao)
            recordAo(cmd, v, s, vpF);
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
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = v.depth.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {0.0f, 0}; // reversed-Z

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
            p.display.x = s.gamma;
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
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipeline);
            vkCmdPushConstants(cmd, gridLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GridPush), &gp);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

        drawImagePlanes(cmd, s, vpF, ImagePlaneDraw::Depth::Behind, false);
        drawContactShadows(cmd, s, vpF);
        // Opaque planes in the scene write depth before the meshes; see-through ones blend over them afterwards.
        drawImagePlanes(cmd, s, vpF, ImagePlaneDraw::Depth::Scene, false);
        drawMeshes(cmd, s, vpF, ao ? v.aoSampleSet : whiteAoSet);
        drawImagePlanes(cmd, s, vpF, ImagePlaneDraw::Depth::Scene, true);
        drawImagePlanes(cmd, s, vpF, ImagePlaneDraw::Depth::Front, false);

        if (linesPipeline && !s.lines.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, linesPipeline);
            vkCmdPushConstants(cmd, linesLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &vpF);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &f.lines, &offset);
            vkCmdDraw(cmd, uint32_t(s.lines.size()), 1, 0, 0);
        }

        VkPipeline biased[2] = {linesOnTopPipeline, tris3dPipeline};
        for (int i = 0; i < 2; ++i)
        {
            const std::vector<LineVertex>& verts = *lineSets[i + 1];
            if (!biased[i] || verts.empty())
                continue;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, biased[i]);
            vkCmdPushConstants(cmd, linesLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &vpF);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &f.lines, &offset);
            vkCmdDraw(cmd, uint32_t(verts.size()), 1, lineFirst[i + 1], 0);
        }

        if (overlayPipeline && !s.overlay.empty())
        {
            glm::mat4 identity(1.0f);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline);
            vkCmdPushConstants(cmd, linesLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &identity);
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
        timestamp(cmd, f, 3);
        v.timing.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
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
            Material m;
            m.baseColorMap = p.image;
            VkDescriptorSet set = materialSet(&m);
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
            for (int c = 0; c < 8; ++c)
            {
                glm::vec3 corner((c & 1) ? d.mesh->boundsMax.x : d.mesh->boundsMin.x, (c & 2) ? d.mesh->boundsMax.y : d.mesh->boundsMin.y,
                                 (c & 4) ? d.mesh->boundsMax.z : d.mesh->boundsMin.z);
                glm::vec3 w(d.world * glm::vec4(corner, 1.0f));
                lo = glm::min(lo, w);
                hi = glm::max(hi, w);
            }
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

    /** One material section of a shaded mesh with whichever mesh pipeline is bound. */
    void drawSection(VkCommandBuffer cmd, const FrameState& s, const MeshDraw& d, GpuMesh& g, const MeshSection& sec, const glm::mat4& viewProj,
                     VkDescriptorSet& boundSet)
    {
        const Material* mat = sec.material < d.materials.size() ? &d.materials[sec.material] : nullptr;
        MeshPush p;
        p.viewProj = viewProj;
        p.world = d.world;
        p.eye = glm::vec4(glm::vec3(s.eye), glm::determinant(glm::mat3(d.world)) < 0.0f ? -1.0f : 1.0f);
        float lighting = s.lighting == FrameState::Lighting::Flat ? 2.0f
                         : s.lighting == FrameState::Lighting::Environment && envLoaded ? 1.0f
                                                                                        : 0.0f;
        p.params = glm::vec4(s.exposure, s.zUp ? 1.0f : 0.0f, 0.0f, lighting);
        p.env = envParams(s);
        p.display.x = s.gamma;
        if (mat)
        {
            p.base = glm::vec4(mat->baseColor, mat->roughness);
            p.surface = glm::vec4(mat->metallic, mat->specular, mat->opacity, mat->opacityClip);
            p.emissive = mat->emissive;
            p.flags = mat->flags;
            // Maps whose upload failed fall back to the constants.
            if (textureKey(mat->baseColorMap, 0) == 0)
                p.flags &= ~MatBaseColorMap;
            if (textureKey(mat->normalMap, 0) == 0)
                p.flags &= ~(MatNormalMap | MatNormalGreenUp);
            if (textureKey(mat->ormMap, 0) == 0)
                p.flags &= ~(MatMetallicMap | MatRoughnessMap | MatOcclusionMap);
            if (textureKey(mat->emissiveMap, 0) == 0)
                p.flags &= ~MatEmissiveMap;
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
        if (s.xray)
        {
            p.flags |= MatTranslucent;
            p.surface.z *= kXrayOpacity;
            p.surface.w *= kXrayOpacity; // masked cutouts stay where they were
        }
        VkDescriptorSet set = materialSet(mat);
        if (!set)
            return;
        if (set != boundSet)
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 1, 1, &set, 0, nullptr);
            boundSet = set;
        }
        vkCmdSetCullMode(cmd, (p.flags & MatTwoSided) && !s.backfaceCulling ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
        vkCmdPushConstants(cmd, meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g.vertices.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, g.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        uint32_t first = std::min(sec.firstIndex, g.indexCount);
        vkCmdDrawIndexed(cmd, std::min(sec.indexCount, g.indexCount - first), 1, first, 0, 0);
    }

    void drawMeshes(VkCommandBuffer cmd, const FrameState& s, const glm::mat4& viewProj, VkDescriptorSet aoSet)
    {
        LILA_ZONE();

        if (s.meshes.empty() || !meshPipeline)
            return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 0, 1, &envSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout, 2, 1, &aoSet, 0, nullptr);

        struct Pending
        {
            const MeshDraw* draw;
            MeshSection section;
            float distance;
        };
        std::vector<Pending> translucent;
        VkDescriptorSet boundSet = VK_NULL_HANDLE;

        for (const MeshDraw& d : s.meshes)
        {
            if (!(d.flags & DrawShaded) || !d.mesh || d.mesh->indices.empty())
                continue;
            GpuMesh* g = gpuMesh(d.mesh);
            std::vector<MeshSection> sections = d.mesh->sections;
            if (sections.empty())
                sections.push_back({0, 0, g->indexCount});
            for (const MeshSection& sec : sections)
            {
                const Material* mat = sec.material < d.materials.size() ? &d.materials[sec.material] : nullptr;
                if (s.xray || (mat && (mat->flags & MatTranslucent)))
                {
                    glm::vec3 centre(d.world * glm::vec4((d.mesh->boundsMin + d.mesh->boundsMax) * 0.5f, 1.0f));
                    translucent.push_back({&d, sec, glm::length(centre - glm::vec3(s.eye))});
                    continue;
                }
                drawSection(cmd, s, d, *g, sec, viewProj, boundSet);
            }
        }
        // Translucency after everything opaque, far to near, testing depth without writing it.
        if (!translucent.empty() && meshBlendPipeline)
        {
            std::ranges::stable_sort(translucent, [](const Pending& a, const Pending& b) { return a.distance > b.distance; });
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshBlendPipeline);
            for (const Pending& t : translucent)
                drawSection(cmd, s, *t.draw, *gpuMesh(t.draw->mesh), t.section, viewProj, boundSet);
        }
        evictTextures();

        if (!wirePipeline)
            return;
        for (const MeshDraw& d : s.meshes)
        {
            if (!d.mesh || d.mesh->edgePoints.empty())
                continue;
            bool wire = (d.flags & DrawWire) || (d.flags & (DrawSelected | DrawUnderSelected | DrawPreselect));
            if (!wire)
                continue;
            GpuMesh* g = gpuMesh(d.mesh);
            WirePush p;
            p.viewProj = viewProj;
            p.world = d.world;
            // Grayscale only: white for the selection, mid gray under it, near black otherwise.
            p.color = (d.flags & DrawSelected) ? glm::vec4(0.92f, 0.92f, 0.92f, 1.0f)
                      : (d.flags & DrawPreselect)     ? glm::vec4(0.75f, 0.75f, 0.75f, 1.0f)
                      : (d.flags & DrawUnderSelected) ? glm::vec4(0.55f, 0.55f, 0.55f, 1.0f)
                      : d.wireColor.a > 0.0f          ? d.wireColor
                                                      : glm::vec4(0.02f, 0.02f, 0.02f, 1.0f);
            bool shaded = d.flags & (DrawShaded | DrawWireDepth);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shaded ? wirePipeline : wireXrayPipeline);
            vkCmdPushConstants(cmd, wireLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &g->edges.buffer, &offset);
            vkCmdDraw(cmd, g->edgePointCount, 1, 0, 0);
        }
        evictMeshes();
    }

    void loop()
    {
        LILA_THREAD_NAME("Render");
        std::unique_lock lock(mutex);
        while (true)
        {
            cv.wait(lock, [&] {
                if (quit || !toRemove.empty())
                    return true;
                // Captures wait out an environment build in flight, so they are lit like the views.
                if (!offscreenJobs.empty() && !envBusy)
                    return true;
                for (auto& [id, v] : viewports)
                    if (v->pending)
                        return true;
                return false;
            });
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
    m->destroyPipelines();
    m->destroyDescriptors();
    m->vk.shutdown();
    m->started = false;
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

std::shared_ptr<const EnvProbe> Renderer::environmentProbe(const std::string& path) const
{
    std::scoped_lock lock(m->probeMutex);
    return m->probePath == path ? m->probe : nullptr;
}
} // namespace lila::render
