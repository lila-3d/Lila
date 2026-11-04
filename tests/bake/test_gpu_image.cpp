#include <doctest/doctest.h>

#include "render/Compute.h"
#include "render/GpuImage.h"
#include "render/Renderer.h"
#include "render/VulkanContext.h"

#include <array>
#include <cstring>

using namespace lila;
using namespace lila::render;

namespace
{
bool gpu()
{
    Renderer& r = Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for GPU image tests");
    return false;
}

std::vector<uint8_t> solid(int w, int h, std::array<uint8_t, 4> c)
{
    std::vector<uint8_t> out;
    for (int i = 0; i < w * h; ++i)
        out.insert(out.end(), c.begin(), c.end());
    return out;
}

TexturePtr texels(int w, int h, std::array<uint8_t, 4> c)
{
    auto t = std::make_shared<TextureImage>();
    t->width = w;
    t->height = h;
    t->rgba = solid(w, h, c);
    return t;
}

/** A flat-lit quad over the whole target, U running 0 to uMax left to right, with one material. */
FrameState quad(const Material& mat, float uMax)
{
    auto mesh = std::make_shared<RenderMesh>();
    const glm::vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const glm::vec2& c : corners)
    {
        MeshVertex v{};
        v.pos = glm::vec3(c, 0.5f);
        v.normal = {0, 0, -1};
        v.tangent = {1, 0, 0, 1};
        v.uv = {(c.x * 0.5f + 0.5f) * uMax, c.y * 0.25f + 0.5f};
        mesh->vertices.push_back(v);
    }
    mesh->indices = {0, 1, 2, 0, 2, 3};
    mesh->boundsMin = {-1, -1, 0.5f};
    mesh->boundsMax = {1, 1, 0.5f};
    FrameState s;
    s.grid = false;
    s.envBackground = false;
    s.ambientOcclusion = false;
    s.groundShadow = 0.0f;
    s.msaaSamples = 1;
    s.lighting = FrameState::Lighting::Flat;
    MeshDraw d;
    d.mesh = mesh;
    d.materials = {mat};
    s.meshes.push_back(d);
    return s;
}

/** BGRA of the pixel at (x, y). */
std::array<int, 4> pixel(const std::vector<uint8_t>& bgra, int width, int x, int y)
{
    const uint8_t* p = &bgra[(size_t(y) * size_t(width) + size_t(x)) * 4];
    return {p[0], p[1], p[2], p[3]};
}

bool reddish(std::array<int, 4> p) { return p[2] > 150 && p[1] < 80 && p[0] < 80; }
bool greenish(std::array<int, 4> p) { return p[1] > 150 && p[2] < 80 && p[0] < 80; }
bool bluish(std::array<int, 4> p) { return p[0] > 150 && p[1] < 80 && p[2] < 80; }
} // namespace

TEST_CASE("GpuImage: updates land in order and read back")
{
    if (!gpu())
        return;
    GpuImageRef img = GpuImage::create({8, 4, TextureFormat::RGBA8, false, true, "test"});
    CHECK(readGpuImage(img) == std::vector<uint8_t>(8 * 4 * 4, 0));

    img->update(0, 0, 8, 4, solid(8, 4, {10, 20, 30, 40}));
    img->update(2, 1, 3, 2, solid(3, 2, {200, 201, 202, 203}));
    const std::vector<uint8_t> back = readGpuImage(img);
    REQUIRE(back.size() == 8 * 4 * 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x)
        {
            const bool inside = x >= 2 && x < 5 && y >= 1 && y < 3;
            CHECK(back[(size_t(y) * 8 + size_t(x)) * 4] == (inside ? 200 : 10));
        }

    CHECK_THROWS(img->update(6, 0, 3, 1, solid(3, 1, {0, 0, 0, 0})));
    CHECK_THROWS(img->update(0, 0, 2, 2, solid(1, 1, {0, 0, 0, 0})));
    CHECK_THROWS(GpuImage::create({0, 4}));
}

TEST_CASE("GpuImage: float and single-channel formats keep their texels")
{
    if (!gpu())
        return;
    GpuImageRef half = GpuImage::create({3, 2, TextureFormat::RGBA16F});
    std::vector<float> values;
    for (int i = 0; i < 6; ++i)
        values.insert(values.end(), {float(i) * 0.5f, 2.0f, -1.0f, 0.25f});
    half->updateRgba(0, 0, 3, 2, values);
    std::vector<float> back;
    decodeTexels(TextureFormat::RGBA16F, false, readGpuImage(half), back);
    CHECK(back == values);

    GpuImageRef r32 = GpuImage::create({2, 2, TextureFormat::R32F});
    r32->updateRgba(1, 1, 1, 1, std::vector<float>{3.5f, 0, 0, 0});
    std::vector<uint8_t> raw = readGpuImage(r32);
    REQUIRE(raw.size() == 16);
    float last = 0.0f;
    std::memcpy(&last, &raw[12], 4);
    CHECK(last == 3.5f);
}

TEST_CASE("GpuImage: compute writes through the storage view")
{
    if (!gpu())
        return;
    GpuImageRef img = GpuImage::create({4, 4, TextureFormat::RGBA8, true});
    const uint64_t before = img->version();
    bool prepared = false, storage = false;
    // A clear between the write calls stands in for a compute producer. Tests call no Vulkan functions themselves: the
    // loader's entry points live in the renderer's library.
    Renderer::get()
        .runTask([&](VulkanContext& vk) {
            compute::Submitter sub;
            sub.init(vk);
            std::vector<compute::Buffer> staging;
            sub.run(vk, [&](VkCommandBuffer cmd) {
                prepared = img->prepare(vk, cmd, staging);
                storage = img->storageView() != VK_NULL_HANDLE;
                if (!prepared)
                    return;
                img->beginWrite(cmd);
                img->recordClear(cmd, {1.0f, 0.0f, 0.0f, 1.0f});
                img->endWrite(cmd);
            });
            for (compute::Buffer& b : staging)
                compute::destroy(vk, b);
            sub.destroy(vk);
            return true;
        })
        .get();
    REQUIRE(prepared);
    CHECK(storage);
    CHECK(img->version() > before);
    const std::vector<uint8_t> back = readGpuImage(img);
    REQUIRE(back.size() == 64);
    CHECK(back[0] == 255);
    CHECK(back[1] == 0);
}

TEST_CASE("Viewport: a UDIM set shows each tile where floor(uv) puts it")
{
    if (!gpu())
        return;
    auto set = std::make_shared<TextureImage>();
    set->width = set->height = 2;
    set->tiles = {{1001, texels(2, 2, {255, 0, 0, 255})}, {1002, texels(2, 2, {0, 255, 0, 255})}};
    Material mat;
    mat.baseColorMap = set;
    mat.flags = MatBaseColorMap | MatTwoSided;
    std::vector<uint8_t> bgra;
    std::string err;
    REQUIRE(Renderer::get().renderOffscreen(quad(mat, 2.0f), 64, 16, bgra, &err));
    CHECK(reddish(pixel(bgra, 64, 8, 8)));
    // The fixed sampler path shows the first tile everywhere; bindless picks the tile.
    const bool bindless = Renderer::get().context()->descriptorIndexing;
    const std::string mode = bindless ? "material textures: bindless" : "material textures: fixed samplers";
    MESSAGE(mode);
    if (bindless)
        CHECK(greenish(pixel(bgra, 64, 56, 8)));
    else
        CHECK(reddish(pixel(bgra, 64, 56, 8)));
}

TEST_CASE("Viewport: float textures and GPU images sample as material maps")
{
    if (!gpu())
        return;
    auto half = std::make_shared<TextureImage>();
    half->width = half->height = 2;
    half->format = TextureFormat::RGBA16F;
    half->srgb = false;
    std::vector<float> green;
    for (int i = 0; i < 4; ++i)
        green.insert(green.end(), {0.0f, 1.0f, 0.0f, 1.0f});
    encodeTexels(TextureFormat::RGBA16F, false, green, half->rgba);
    Material mat;
    mat.baseColorMap = half;
    mat.flags = MatBaseColorMap | MatTwoSided;
    std::vector<uint8_t> bgra;
    REQUIRE(Renderer::get().renderOffscreen(quad(mat, 1.0f), 16, 16, bgra));
    CHECK(greenish(pixel(bgra, 16, 8, 8)));

    GpuImageRef img = GpuImage::create({4, 4, TextureFormat::RGBA8, true, true});
    img->update(0, 0, 4, 4, solid(4, 4, {255, 0, 0, 255}));
    mat.baseColorMap = img->asTexture();
    CHECK(mat.baseColorMap == img->asTexture());
    REQUIRE(Renderer::get().renderOffscreen(quad(mat, 1.0f), 16, 16, bgra));
    CHECK(reddish(pixel(bgra, 16, 8, 8)));
    img->update(0, 0, 4, 4, solid(4, 4, {0, 0, 255, 255}));
    REQUIRE(Renderer::get().renderOffscreen(quad(mat, 1.0f), 16, 16, bgra));
    CHECK(bluish(pixel(bgra, 16, 8, 8)));
}

TEST_CASE("Viewport: the shader wire draws on polygon sides, over a clear surface too")
{
    if (!gpu())
        return;
    Material mat;
    mat.baseColor = glm::vec3(0.0f, 0.0f, 1.0f);
    for (bool dynamic : {false, true})
        for (float opacity : {1.0f, 0.0f})
        {
            CAPTURE(dynamic);
            CAPTURE(opacity);
            Material m = mat;
            if (opacity < 1.0f)
            {
                m.flags |= MatTranslucent;
                m.opacity = opacity;
            }
            FrameState s = quad(m, 1.0f);
            auto mesh = std::make_shared<RenderMesh>(*s.meshes[0].mesh);
            // Triangle 0 (0,1,2): sides 0-1 and 1-2 are the quad's; triangle 1 (0,2,3): sides 2-3 and 3-0.
            mesh->edgeMask = {0b011u | (0b110u << 4)};
            if (dynamic)
            {
                mesh->dynamic = std::make_shared<DynamicVertices>();
                mesh->dynamic->vertices.assign(mesh->vertices.begin(), mesh->vertices.end());
                mesh->vertices.clear();
            }
            MeshDraw d = s.meshes[0];
            d.mesh = mesh;
            d.flags = DrawShaded | DrawWire;
            d.wireColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            d.wireWidth = 6.0f;
            s.meshes = {d};
            std::vector<uint8_t> bgra;
            std::string err;
            REQUIRE(Renderer::get().renderOffscreen(s, 32, 32, bgra, &err));
            CHECK(reddish(pixel(bgra, 32, 16, 0)));
            CHECK(reddish(pixel(bgra, 32, 0, 16)));
            CHECK_FALSE(reddish(pixel(bgra, 32, 16, 16)));
            if (opacity == 1.0f)
                CHECK(bluish(pixel(bgra, 32, 16, 16)));
        }
}

TEST_CASE("Compute: the shared pipeline cache makes each kernel once")
{
    if (!gpu())
        return;
    const std::vector<uint32_t> code = compute::loadSpirv("bake_dilate.spv");
    REQUIRE(!code.empty());
    VkPipeline a = VK_NULL_HANDLE, b = VK_NULL_HANDLE, c = VK_NULL_HANDLE;
    size_t hits = 0, misses = 0, left = 1;
    Renderer::get()
        .runTask([&](VulkanContext& vk) {
            const uint32_t push = std::min(256u, vk.properties.limits.maxPushConstantsSize);
            VkPipelineLayout layout = compute::pipelineLayout(vk, push, VK_SHADER_STAGE_COMPUTE_BIT);
            const uint64_t key = compute::layoutKey(push, VK_SHADER_STAGE_COMPUTE_BIT, "test");
            compute::PipelineCache cache;
            a = cache.get(vk, layout, key, code, "jfaInitCS");
            b = cache.get(vk, layout, key, code, "jfaInitCS");
            c = cache.get(vk, layout, key, code, "jfaStepCS");
            hits = cache.hits();
            misses = cache.misses();
            cache.clear(vk);
            left = cache.size();
            compute::destroy(vk, layout);
            return true;
        })
        .get();
    CHECK(a);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(misses == 2);
    CHECK(hits == 1);
    CHECK(left == 0);
}
