#include "render/Backdrop.h"
#include "scene/Camera.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>

using namespace lila;

namespace
{
/** A cube whose upper hemisphere (+Y) glows at `sky` and lower half sits at `ground`. */
render::EnvironmentData twoToneEnvironment(float sky, float ground, int size = 64, int mips = 4)
{
    render::EnvironmentData env;
    env.size = size;
    for (int m = 0; m < mips; ++m)
    {
        int n = std::max(1, size >> m);
        std::vector<uint16_t> px(size_t(n) * n * 6 * 4);
        for (int face = 0; face < 6; ++face)
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                {
                    // Vulkan faces: +Y is face 2, -Y face 3; the side faces' rows run top (+Y) to bottom.
                    bool up = face == 2 || (face != 3 && y < n / 2);
                    float v = up ? sky : ground;
                    size_t i = ((size_t(face) * n + y) * n + x) * 4;
                    px[i] = px[i + 1] = px[i + 2] = glm::packHalf1x16(v);
                    px[i + 3] = glm::packHalf1x16(1.0f);
                }
        env.mips.push_back(std::move(px));
    }
    return env;
}

render::FrameState lookingAlongMinusZ()
{
    render::FrameState s;
    s.view = glm::lookAt(glm::dvec3(0.0), glm::dvec3(0, 0, -1), glm::dvec3(0, 1, 0));
    s.proj = scene::camera::projection({0.6, 0.4}, false, 0.1, 0.0);
    s.envBackground = true;
    s.envIntensity = 1.0f;
    s.envBackgroundExposure = 1.0f;
    s.envBlur = 0.0f;
    s.exposure = 1.0f;
    s.gamma = 1.0f;
    return s;
}
} // namespace

TEST_CASE("backdrop luminance follows the flat background without an environment")
{
    render::FrameState s = lookingAlongMinusZ();
    s.background = {0.3f, 0.3f, 0.3f};
    std::vector<float> dark = render::backdropLuminance(s, nullptr, 4, 3);
    REQUIRE(dark.size() == 12);
    CHECK(dark[0] == doctest::Approx(0.0732).epsilon(0.02));
    CHECK(dark[0] < 0.18f);

    s.background = {0.85f, 0.85f, 0.85f};
    CHECK(render::backdropLuminance(s, nullptr, 1, 1)[0] > 0.6f);

    // Not drawing the environment shows the flat color even when one is loaded.
    auto probe = render::makeEnvProbe(twoToneEnvironment(4.0f, 4.0f));
    s.background = {0.3f, 0.3f, 0.3f};
    s.envBackground = false;
    CHECK(render::backdropLuminance(s, probe.get(), 1, 1)[0] == doctest::Approx(dark[0]));
}

TEST_CASE("backdrop luminance reads the environment as displayed")
{
    auto probe = render::makeEnvProbe(twoToneEnvironment(2.0f, 0.02f), 32);
    REQUIRE(probe);
    CHECK(probe->firstMip == 1);
    CHECK(probe->sizes.front() == 32);

    render::FrameState s = lookingAlongMinusZ();
    std::vector<float> lum = render::backdropLuminance(s, probe.get(), 1, 4);
    // Top row looks up into the bright half, bottom row down at the dark floor.
    CHECK(lum.front() > 0.6f);
    CHECK(lum.back() < 0.05f);

    // The backdrop exposure dims what is shown.
    s.envBackgroundExposure = 0.05f;
    std::vector<float> dimmed = render::backdropLuminance(s, probe.get(), 1, 4);
    CHECK(dimmed.front() < lum.front());
    CHECK(dimmed.front() < 0.18f);

    // In a Z-up scene world -Z is the environment's down, so the same camera now looks at the floor.
    s.envBackgroundExposure = 1.0f;
    s.zUp = true;
    std::vector<float> zUp = render::backdropLuminance(s, probe.get(), 1, 4);
    CHECK(zUp.front() < 0.05f);
}

TEST_CASE("probe sampling blends mips by lod")
{
    render::EnvironmentData env = twoToneEnvironment(1.0f, 1.0f, 16, 3);
    // Make the last mip distinct so a blend shows.
    for (size_t i = 0; i < env.mips[2].size(); i += 4)
        env.mips[2][i] = env.mips[2][i + 1] = env.mips[2][i + 2] = glm::packHalf1x16(3.0f);
    auto probe = render::makeEnvProbe(env, 64);
    REQUIRE(probe);
    CHECK(probe->firstMip == 0);
    CHECK(render::sampleProbe(*probe, {0, 0, -1}, 0.0f).r == doctest::Approx(1.0f));
    CHECK(render::sampleProbe(*probe, {0, 0, -1}, 1.5f).r == doctest::Approx(2.0f));
    CHECK(render::sampleProbe(*probe, {0, 0, -1}, 9.0f).r == doctest::Approx(3.0f));
}
