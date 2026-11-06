// SPDX-License-Identifier: Apache-2.0
// Static (static_op.cpp): hair charged with static electricity repels itself. Charge on a grid,
// its potential solved, each point moved along the field by its own charge (more toward the
// tips), plus a field below the grid's scale that splays a lock into its strands.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv, read-only
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#include "groomlab/graph.h"
#include "groomlab/operators.h"
#include "groomlab/preview_core.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265f;

StrandsPtr charge(const StrandsPtr& in, const MeshPtr& scalp, const std::map<std::string, ParamValue>& params = {}) {
    auto node = OperatorRegistry::instance().create(op::kStatic);
    for (const auto& [name, value] : params) node->params().set(name, value);
    ExternalInputs ext;
    if (scalp) ext.set("scalp", scalp, 1);
    EvalContext ctx;
    ctx.externals = &ext;
    return valueAs<StrandsPtr>(node->evaluate(ctx, {Value(in)}));
}

// Long hair combed over a sphere (radius 10 x scale, the scalp) in clumps: `count` strands rooted
// over its top (within 60 degrees of the pole, a Fibonacci spread), each 18 x scale long, lying
// 0.05 x scale above the skin down its meridian to the equator, then hanging straight down; each
// strand drawn toward the strand rooted at its clump's center (the nearest of `clumps` such, a
// Fibonacci spread of their own) by `clumping` at the tip, none at the root: locks of hair.
struct SphereHair {
    MeshPtr scalp;
    StrandsPtr strands;
    std::vector<std::size_t> clumpOf;  // per strand
    std::size_t clumps = 0;
};
constexpr std::size_t kPoints = 16;
void combed(float theta0, float phi, float scale, std::vector<Vec3f>* points) {
    const float radius = 10.0f * scale, lift = 0.05f * scale, length = 18.0f * scale;
    const float r = radius + lift;
    const float arc = r * (0.5f * kPi - theta0);  // to the equator
    for (std::size_t k = 0; k < kPoints; ++k) {
        const float along = length * static_cast<float>(k) / static_cast<float>(kPoints - 1);
        if (along <= arc) {
            const float theta = theta0 + along / r;
            const float rr = k == 0 ? radius : r;  // the root on the skin
            (*points)[k] = Vec3f(rr * std::sin(theta) * std::cos(phi), rr * std::cos(theta), rr * std::sin(theta) * std::sin(phi));
        } else {
            (*points)[k] = Vec3f(r * std::cos(phi), -(along - arc), r * std::sin(phi));
        }
    }
}
SphereHair clumpedOverSphere(std::size_t count, std::size_t clumps = 150, float scale = 1.0f, float clumping = 0.85f) {
    SphereHair h;
    h.scalp = test::makeSphere(48, 96, 10.0f * scale);
    h.clumps = clumps;
    auto fibonacci = [](std::size_t i, std::size_t n, float* theta0, float* phi) {
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        *theta0 = std::acos(1.0f - u * 0.5f);  // cos(theta) from 1 down to cos(60 degrees)
        *phi = static_cast<float>(i) * 2.39996323f;
    };
    std::vector<std::vector<Vec3f>> centers(clumps, std::vector<Vec3f>(kPoints));
    for (std::size_t c = 0; c < clumps; ++c) {
        float t, p;
        fibonacci(c, clumps, &t, &p);
        combed(t, p, scale, &centers[c]);
    }
    auto s = std::make_shared<StrandSet>();
    std::vector<Vec3f> points(kPoints);
    for (std::size_t i = 0; i < count; ++i) {
        float t, p;
        fibonacci(i, count, &t, &p);
        combed(t, p, scale, &points);
        std::size_t best = 0;
        float nearest = 1e30f;
        for (std::size_t c = 0; c < clumps; ++c) {
            const float d = (centers[c][0] - points[0]).length();
            if (d < nearest) {
                nearest = d;
                best = c;
            }
        }
        for (std::size_t k = 1; k < kPoints; ++k) {
            const float pull = clumping * static_cast<float>(k) / static_cast<float>(kPoints - 1);
            Vec3f& p = points[k];
            p = p + (centers[best][k] - p) * pull;
            // Kept over the skin (a pull across the pole would cut through the head).
            const float r = 10.05f * scale;
            if (p.y >= 0.0f && p.length() < r) p = p * (r / p.length());
            const float side = std::sqrt(p.x * p.x + p.z * p.z);
            if (p.y < 0.0f && side < r && side > 0.0f) p = Vec3f(p.x * r / side, p.y, p.z * r / side);
        }
        h.clumpOf.push_back(best);
        s->addStrand(static_cast<StrandId>(i), points, {}, h.scalp->closestPoint(points[0]).root);
    }
    h.strands = s;
    return h;
}

// Over the skin: from the sphere above the equator, from its widest circle below.
float over(const Vec3f& x, float radius = 10.0f) {
    return x.y >= 0.0f ? x.length() - radius : std::sqrt(x.x * x.x + x.z * x.z) - radius;
}

// The mean height of the tips over the skin.
double tipHeight(const StrandSet& s) {
    double sum = 0.0;
    for (std::size_t k = 0; k < s.strandCount(); ++k) sum += over(s.strandPoints(k)[s.strandPointCount(k) - 1]);
    return sum / static_cast<double>(s.strandCount());
}

// The locks' spread at point `at` along their strands: each clump's strands' RMS distance from
// their mean there, averaged over the clumps.
double lockSpread(const SphereHair& h, const StrandSet& s, std::size_t at) {
    std::vector<Vec3f> mean(h.clumps);
    std::vector<double> n(h.clumps, 0.0), sq(h.clumps, 0.0);
    for (std::size_t k = 0; k < s.strandCount(); ++k) {
        mean[h.clumpOf[k]] += s.strandPoints(k)[at];
        n[h.clumpOf[k]] += 1.0;
    }
    for (std::size_t c = 0; c < h.clumps; ++c) mean[c] = mean[c] / static_cast<float>(std::max(n[c], 1.0));
    for (std::size_t k = 0; k < s.strandCount(); ++k) {
        const Vec3f d = s.strandPoints(k)[at] - mean[h.clumpOf[k]];
        sq[h.clumpOf[k]] += static_cast<double>(d.dot(d));
    }
    double sum = 0.0;
    std::size_t used = 0;
    for (std::size_t c = 0; c < h.clumps; ++c) {
        if (n[c] < 2.0) continue;
        sum += std::sqrt(sq[c] / n[c]);
        ++used;
    }
    return sum / static_cast<double>(std::max<std::size_t>(used, 1));
}

TEST(Static, StrengthZeroIsTheInput) {
    const SphereHair hair = clumpedOverSphere(2000);
    EXPECT_EQ(charge(hair.strands, hair.scalp, {{"strength", 0.0}}), hair.strands);
}

TEST(Static, ALockFraysFromItsTips) {
    // Clumped locks splay into their strands: the spread of a lock's strands at the tips grows
    // (and more than at the middle), roots stay bit for bit and every segment keeps its length.
    const SphereHair hair = clumpedOverSphere(20000);
    const StrandsPtr out = charge(hair.strands, hair.scalp);
    ASSERT_TRUE(out && out != hair.strands);
    const std::size_t tip = kPoints - 1, mid = kPoints / 2;
    const double tipBefore = lockSpread(hair, *hair.strands, tip), tipAfter = lockSpread(hair, *out, tip);
    const double midBefore = lockSpread(hair, *hair.strands, mid), midAfter = lockSpread(hair, *out, mid);
    std::printf("[ static ] lock spread at the tips %.3f before, %.3f after; at the middle %.3f before, %.3f after\n", tipBefore,
                tipAfter, midBefore, midAfter);
    EXPECT_GT(tipAfter, 1.5 * tipBefore);
    EXPECT_GT(tipAfter - tipBefore, midAfter - midBefore);
    float worst = 0.0f;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f* p = hair.strands->strandPoints(s);
        const Vec3f* q = out->strandPoints(s);
        EXPECT_EQ(std::memcmp(&p[0], &q[0], sizeof(Vec3f)), 0) << "root of strand " << s;
        for (std::size_t i = 1; i < out->strandPointCount(s); ++i) {
            const float a = (p[i] - p[i - 1]).length(), b = (q[i] - q[i - 1]).length();
            worst = std::max(worst, std::fabs(a - b) / a);
        }
    }
    EXPECT_LT(worst, 1e-4f);
}

TEST(Static, HairOverASpherePuffsOutAndStaysOutOfIt) {
    // The tips lift off the head (their mean height over the skin grows), the roots' end barely
    // moves, and no point goes into the sphere.
    const SphereHair hair = clumpedOverSphere(20000);
    const StrandsPtr out = charge(hair.strands, hair.scalp);
    ASSERT_TRUE(out && out != hair.strands);
    float lowest = 1e30f;
    double nearRoot = 0.0;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f* q = out->strandPoints(s);
        for (std::size_t i = 1; i < out->strandPointCount(s); ++i) lowest = std::min(lowest, q[i].length() - 10.0f);
        nearRoot += (q[1] - hair.strands->strandPoints(s)[1]).length();
    }
    nearRoot /= static_cast<double>(out->strandCount());
    const double before = tipHeight(*hair.strands), after = tipHeight(*out);
    std::printf("[ static ] tips over the skin %.3f before, %.3f after; first point moved %.4f; lowest point %.4f over the skin\n", before,
                after, nearRoot, lowest);
    EXPECT_GT(after, before + 1.0);
    EXPECT_LT(nearRoot, 0.1 * (after - before));
    EXPECT_GT(lowest, -0.01f);  // (the faceted sphere's faces lie up to 0.005 inside the round one)
}

TEST(Static, MoreStrengthPuffsMore) {
    const SphereHair hair = clumpedOverSphere(10000);
    double last = tipHeight(*hair.strands);
    for (const double strength : {0.25, 0.5, 1.0, 2.0}) {
        const StrandsPtr out = charge(hair.strands, hair.scalp, {{"strength", strength}});
        ASSERT_TRUE(out && out != hair.strands);
        const double height = tipHeight(*out);
        std::printf("[ static ] strength %.2f: tips %.3f over the skin\n", strength, height);
        EXPECT_GT(height, last + 0.1) << "at strength " << strength;
        last = height;
    }
}

TEST(Static, ScaleInvariant) {
    // The hair 100 times as big comes out the same, 100 times as big.
    const SphereHair small = clumpedOverSphere(3000, 60, 1.0f), big = clumpedOverSphere(3000, 60, 100.0f);
    const StrandsPtr a = charge(small.strands, small.scalp), b = charge(big.strands, big.scalp);
    ASSERT_TRUE(a && b && a != small.strands);
    float worst = 0.0f, moved = 0.0f;
    for (std::size_t i = 0; i < a->pointCount(); ++i) {
        worst = std::max(worst, (b->positions()[i] / 100.0f - a->positions()[i]).length());
        moved = std::max(moved, (a->positions()[i] - small.strands->positions()[i]).length());
    }
    std::printf("[ static ] at 100x: %.5f apart (moved up to %.3f)\n", worst, moved);
    EXPECT_GT(moved, 1.0f);
    EXPECT_LT(worst, 5e-3f);  // (of a sphere of radius 10: the scalp's field has cells of a power of two)
}

TEST(Static, DeterministicOnAnyNumberOfThreads) {
    const SphereHair hair = clumpedOverSphere(20000);
    const StrandsPtr many = charge(hair.strands, hair.scalp);
    ASSERT_TRUE(many && many != hair.strands);
    StrandsPtr one;
#if GROOMLAB_HAS_TBB
    tbb::task_arena single(1);
    single.execute([&] { one = charge(hair.strands, hair.scalp); });
#else
    one = charge(hair.strands, hair.scalp);
#endif
    ASSERT_TRUE(one);
    EXPECT_EQ(std::memcmp(one->positions().data(), many->positions().data(), many->pointCount() * sizeof(Vec3f)), 0);
    const StrandsPtr again = charge(hair.strands, hair.scalp);
    EXPECT_EQ(std::memcmp(again->positions().data(), many->positions().data(), many->pointCount() * sizeof(Vec3f)), 0);
}

TEST(Static, APreviewShowsTheRendersStrands) {
    // A preview holds a share of the strands (and the core every preview holds, preview_core.h):
    // Static's charge is the core's alone, so every strand a preview shows comes out as the
    // render's strand of that ID, bit for bit.
    constexpr std::size_t kCount = 40000;
    const SphereHair hair = clumpedOverSphere(kCount);
    const float core = coreShare(static_cast<double>(kCount));
    ASSERT_LT(core, 1.0f);
    auto held = [&](float keep) {  // the strands a preview at `keep` holds (1: the render's), with their roles
        auto s = std::make_shared<StrandSet>();
        std::vector<float> roles;
        for (std::size_t k = 0; k < hair.strands->strandCount(); ++k) {
            float role = kCoreShown;
            if (!previewRole(hair.strands->ids()[k], keep >= 1.0f ? 2.0f : keep, core, &role)) continue;
            const Vec3f* p = hair.strands->strandPoints(k);
            s->addStrand(hair.strands->ids()[k], std::vector<Vec3f>(p, p + hair.strands->strandPointCount(k)), {},
                         hair.strands->roots()[k]);
            roles.push_back(role);
        }
        s->addStrandChannel(kPreviewChannel, kCoreShown) = roles;
        return StrandsPtr(s);
    };
    const StrandsPtr render = charge(held(1.0f), hair.scalp);
    ASSERT_TRUE(render);
    std::unordered_map<StrandId, std::size_t> at;
    for (std::size_t k = 0; k < render->strandCount(); ++k) at[render->ids()[k]] = k;
    float moved = 0.0f;
    for (std::size_t i = 0; i < render->pointCount(); ++i) moved = std::max(moved, (render->positions()[i] - hair.strands->positions()[i]).length());
    EXPECT_GT(moved, 0.5f);
    for (const float keep : {0.3f, 0.05f}) {
        const StrandsPtr preview = charge(held(keep), hair.scalp);
        ASSERT_TRUE(preview);
        std::size_t different = 0, shown = 0;
        const PreviewCore roles(*preview);
        for (std::size_t k = 0; k < preview->strandCount(); ++k) {
            if (roles.hidden(k)) continue;
            ++shown;
            const std::size_t r = at.at(preview->ids()[k]);
            const std::size_t n = preview->strandPointCount(k);
            if (n != render->strandPointCount(r) || std::memcmp(preview->strandPoints(k), render->strandPoints(r), n * sizeof(Vec3f)) != 0) {
                ++different;
            }
        }
        EXPECT_GT(shown, 1000u);
        EXPECT_EQ(different, 0u) << "at a preview of " << keep;
    }
}

// Not a check: writes the clumped hair calm, then at a moderate and a strong charge (one strand a
// line: x y z per point) to STATIC_PICTURE's folder, for a picture.
TEST(Static, Picture) {
    const char* folder = std::getenv("STATIC_PICTURE");
    if (!folder) GTEST_SKIP() << "set STATIC_PICTURE to a folder";
    const SphereHair hair = clumpedOverSphere(20000);
    auto write = [&](const StrandSet& s, const std::string& name) {
        std::ofstream file(std::string(folder) + "/" + name);
        for (std::size_t k = 0; k < s.strandCount(); ++k) {
            const Vec3f* p = s.strandPoints(k);
            for (std::size_t i = 0; i < s.strandPointCount(k); ++i) file << p[i].x << ' ' << p[i].y << ' ' << p[i].z << ' ';
            file << '\n';
        }
    };
    write(*hair.strands, "static_calm.txt");
    write(*charge(hair.strands, hair.scalp, {{"strength", 0.5}}), "static_moderate.txt");
    write(*charge(hair.strands, hair.scalp, {{"strength", 1.0}}), "static_strong.txt");
}

}  // namespace
}  // namespace groomlab
