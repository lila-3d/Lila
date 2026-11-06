// SPDX-License-Identifier: Apache-2.0
#include "groomlab/lod.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>

#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

namespace {

Vec3f row(const LodCamera& c, int r) { return {c.matrix[r][0], c.matrix[r][1], c.matrix[r][2]}; }

}  // namespace

LodPlan planLod(const StrandSet& strands, const LodCamera& camera, const LodSettings& settings, const ScalpMesh* scalp) {
    LodPlan plan;
    const std::size_t count = strands.strandCount();
    plan.kept.reserve(count);
    plan.widthScale.reserve(count);
    // Camera axes, normalized: the matrix may carry scale (a scaled groom).
    const Vec3f position = row(camera, 3);
    const Vec3f ax = row(camera, 0), ay = row(camera, 1), az = row(camera, 2);
    const float sx = ax.length(), sy = ay.length(), sz = az.length();
    const Vec3f x = sx > 0 ? ax / sx : Vec3f{1, 0, 0}, y = sy > 0 ? ay / sy : Vec3f{0, 1, 0};
    const Vec3f z = sz > 0 ? az / sz : Vec3f{0, 0, 1};
    const float spreadX = camera.tanHalfFovX * (1.0f + 2.0f * settings.margin);
    const float spreadY = camera.tanHalfFovY * (1.0f + 2.0f * settings.margin);
    const float orthoX = std::max(camera.orthoHalfWidth, 0.0f) * (1.0f + 2.0f * settings.margin);
    const float orthoY = std::max(camera.orthoHalfHeight, 0.0f) * (1.0f + 2.0f * settings.margin);
    const bool thin = settings.distance && !camera.orthographic;  // an ortho view has no distance
    const float nearD = std::max(0.0f, settings.nearDistance);
    const float farD = std::max(nearD + 1e-6f, settings.farDistance);
    const float minDensity = std::clamp(settings.minDensity, 0.0f, 1.0f);
    // Backfacing: a root whose normal points away from the camera by more than the padding past
    // side-on (cos of the angle to the camera below -sin(padding)).
    const float facingLimit = -pm::sin(std::clamp(settings.backfaceAngle, 0.0f, 90.0f) * 0.017453292519943f);
    auto facesAway = [&](std::size_t s, const Vec3f* pts, std::size_t n) {
        Vec3f normal;
        const StrandRoot& root = strands.roots()[s];
        if (scalp && scalp->contains(root)) {
            normal = scalp->normal(root);
        } else {
            for (std::size_t k = 1; k < n && normal.length() == 0.0f; ++k) normal = pts[k] - pts[0];
        }
        normal = normal.normalized();
        if (normal.length() == 0.0f) return false;  // no facing: kept
        const Vec3f toCamera = camera.orthographic ? z : (position - pts[0]).normalized();
        return normal.dot(toCamera) < facingLimit;
    };

    auto visible = [&](const Vec3f& p) {
        const Vec3f d = p - position;
        const float depth = -d.dot(z);
        if (depth <= 0.0f) return false;
        if (camera.orthographic) return std::abs(d.dot(x)) <= orthoX && std::abs(d.dot(y)) <= orthoY;
        return std::abs(d.dot(x)) <= depth * spreadX && std::abs(d.dot(y)) <= depth * spreadY;
    };

    // Each strand's fate on its own (0 kept, 1 culled, 2 thinned, 3 facing away), then the kept ones in order.
    std::vector<std::uint8_t> fate(count, 0);
    std::vector<float> widthScale(count, 1.0f);
    const std::vector<StrandId>& ids = strands.ids();
    parallelFor(0, count, [&](std::size_t s) {
        const std::size_t n = strands.strandPointCount(s);
        const Vec3f* pts = strands.strandPoints(s);
        if (settings.frustum && n > 0) {
            bool in = false;
            for (std::size_t k = 0; k < n && !in; ++k) in = visible(pts[k]);
            if (!in) {
                fate[s] = 1;
                return;
            }
        }
        if (settings.backface && n > 0 && s < strands.roots().size() && facesAway(s, pts, n)) {
            fate[s] = 3;
            return;
        }
        float density = 1.0f;
        if (thin && n > 0) {
            const float dist = (pts[0] - position).length();
            const float t = std::clamp((dist - nearD) / (farD - nearD), 0.0f, 1.0f);
            density = 1.0f + (minDensity - 1.0f) * (t * t * (3.0f - 2.0f * t));
            if (unitFloat(hashCombine(ids[s], 0x10D5EED)) >= density) {
                fate[s] = 2;
                return;
            }
        }
        const float scale = density < 1.0f ? pm::pow(std::max(density, 1e-3f), -settings.widthCompensation) : 1.0f;
        widthScale[s] = std::min(scale, 10.0f);  // past 10x ribbons read as cards
    }, 1024);
    for (std::size_t s = 0; s < count; ++s) {
        if (fate[s] == 1) {
            ++plan.culled;
        } else if (fate[s] == 2) {
            ++plan.thinned;
        } else if (fate[s] == 3) {
            ++plan.backfacing;
        } else {
            plan.kept.push_back(s);
            plan.widthScale.push_back(widthScale[s]);
        }
    }
    return plan;
}

StrandSet applyLod(const StrandSet& strands, const LodPlan& plan) {
    StrandSet out = strands.subset(plan.kept);
    std::vector<float>& widths = out.mutableWidths();
    for (std::size_t i = 0; i < plan.kept.size(); ++i) {
        const float scale = plan.widthScale[i];
        if (scale == 1.0f) continue;
        const std::size_t begin = out.strandBegin(i), n = out.strandPointCount(i);
        for (std::size_t k = 0; k < n; ++k) widths[begin + k] *= scale;
    }
    return out;
}

}  // namespace groomlab
