// SPDX-License-Identifier: Apache-2.0
// Lining a camera up with a photo: the pinhole solve from 2D-3D point pairs.

#include <cmath>
#include <random>

#include <gtest/gtest.h>

#include "groomlab/camera_solve.h"

namespace groomlab {
namespace {

constexpr double kW = 1600.0, kH = 1200.0;

double distance(const CameraVec3& a, const CameraVec3& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// The largest difference between two rotations' entries.
double rotationError(const CameraMat3& a, const CameraMat3& b) {
    double e = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) e = std::max(e, std::abs(a[i][j] - b[i][j]));
    return e;
}

// A camera at `eye` looking at `target` (Y up), as Maya would aim one.
CameraPose aim(const CameraVec3& eye, const CameraVec3& target, double rollDegrees = 0.0) {
    CameraVec3 back{eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};  // camera +Z
    const double len = std::sqrt(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
    for (double& c : back) c /= len;
    CameraVec3 right{back[2], 0.0, -back[0]};  // up x back, up = Y
    const double rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
    for (double& c : right) c /= rl;
    CameraVec3 up{back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2],
                  back[0] * right[1] - back[1] * right[0]};
    const double r = rollDegrees * 3.14159265358979323846 / 180.0;
    const CameraVec3 x{right[0] * std::cos(r) + up[0] * std::sin(r), right[1] * std::cos(r) + up[1] * std::sin(r),
                       right[2] * std::cos(r) + up[2] * std::sin(r)};
    const CameraVec3 y{up[0] * std::cos(r) - right[0] * std::sin(r), up[1] * std::cos(r) - right[1] * std::sin(r),
                       up[2] * std::cos(r) - right[2] * std::sin(r)};
    CameraPose pose;
    for (int i = 0; i < 3; ++i) {
        pose.rotation[i][0] = x[i];
        pose.rotation[i][1] = y[i];
        pose.rotation[i][2] = back[i];
    }
    pose.position = eye;
    return pose;
}

// Points on a head-sized lumpy blob around the origin (never a plane).
std::vector<CameraVec3> headPoints(int count, std::mt19937& rng, double flatten = 1.0) {
    std::uniform_real_distribution<double> uni(-1.0, 1.0);
    std::vector<CameraVec3> points;
    while (static_cast<int>(points.size()) < count) {
        CameraVec3 p{uni(rng), uni(rng), uni(rng)};
        const double len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (len < 0.2 || len > 1.0) continue;
        const double r = 10.0 * (0.8 + 0.2 * std::abs(uni(rng)));
        points.push_back({p[0] / len * r, p[1] / len * r * 1.2, p[2] / len * r * flatten});
    }
    return points;
}

std::vector<CameraCorrespondence> project(const CameraPose& pose, double focal, const std::vector<CameraVec3>& points,
                                          double noise = 0.0, unsigned seed = 1) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, noise > 0 ? noise : 1.0);
    std::vector<CameraCorrespondence> out;
    for (const auto& p : points) {
        CameraCorrespondence c;
        c.world = p;
        EXPECT_TRUE(projectToImage(pose, focal, kW, kH, p, c.u, c.v));
        if (noise > 0) {
            c.u += gauss(rng);
            c.v += gauss(rng);
        }
        out.push_back(c);
    }
    return out;
}

CameraSolveOptions options(double focal = 0.0, bool fix = false) {
    CameraSolveOptions o;
    o.imageWidth = kW;
    o.imageHeight = kH;
    o.focalPx = focal;
    o.fixFocal = fix;
    return o;
}

TEST(CameraSolve, ProjectionFollowsMayasCameraAxes) {
    // A camera at +Z looking down -Z: +X lands right of center, +Y above it.
    const CameraPose pose = aim({0, 0, 50}, {0, 0, 0});
    double u = 0, v = 0;
    ASSERT_TRUE(projectToImage(pose, 1000.0, kW, kH, {0, 0, 0}, u, v));
    EXPECT_NEAR(u, 800.0, 1e-9);
    EXPECT_NEAR(v, 600.0, 1e-9);
    ASSERT_TRUE(projectToImage(pose, 1000.0, kW, kH, {5, 5, 0}, u, v));
    EXPECT_NEAR(u, 900.0, 1e-9);
    EXPECT_NEAR(v, 500.0, 1e-9);
    EXPECT_FALSE(projectToImage(pose, 1000.0, kW, kH, {0, 0, 60}, u, v));  // behind it
}

TEST(CameraSolve, RecoversRandomCamerasExactly) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> uni(-1.0, 1.0);
    for (int trial = 0; trial < 12; ++trial) {
        const CameraVec3 dir{uni(rng), 0.6 * uni(rng), uni(rng)};
        const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        const double dist = 45.0 + 40.0 * std::abs(uni(rng));
        const CameraVec3 eye{dir[0] / len * dist, dir[1] / len * dist, dir[2] / len * dist};
        const CameraPose truth = aim(eye, {2.0 * uni(rng), 2.0 * uni(rng), 2.0 * uni(rng)}, 15.0 * uni(rng));
        const double focal = kW * (0.9 + 2.0 * std::abs(uni(rng)));
        const auto pairs = project(truth, focal, headPoints(6 + trial, rng));
        const CameraSolveResult r = solveCamera(pairs, options());
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_LT(r.rmsPx, 1e-4) << "trial " << trial;
        EXPECT_LT(distance(r.pose.position, truth.position), 1e-4 * dist) << "trial " << trial;
        EXPECT_LT(rotationError(r.pose.rotation, truth.rotation), 1e-5) << "trial " << trial;
        EXPECT_NEAR(r.focalPx / focal, 1.0, 1e-5) << "trial " << trial;
        ASSERT_EQ(r.errorsPx.size(), pairs.size());
    }
}

TEST(CameraSolve, NoisyClicksLandClose) {
    std::mt19937 rng(11);
    const CameraPose truth = aim({30, 8, 55}, {0, 1, 0}, 4.0);
    const double focal = 1.6 * kW;
    const auto pairs = project(truth, focal, headPoints(14, rng), 0.7, 3);
    const CameraSolveResult r = solveCamera(pairs, options());
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_GT(r.rmsPx, 0.2);  // the noise is still there
    EXPECT_LT(r.rmsPx, 1.5);
    EXPECT_LT(distance(r.pose.position, truth.position), 0.05 * 63.0);
    EXPECT_NEAR(r.focalPx / focal, 1.0, 0.05);
    EXPECT_LT(rotationError(r.pose.rotation, truth.rotation), 0.02);
}

TEST(CameraSolve, BadStartingGuessStillConverges) {
    std::mt19937 rng(5);
    const CameraPose truth = aim({-40, 20, 35}, {0, 0, 0}, -10.0);
    const double focal = 2.2 * kW;
    const auto pairs = project(truth, focal, headPoints(8, rng));
    CameraSolveOptions o = options(0.6 * kW);  // far off
    o.hasInitialPose = true;
    o.initialPose = aim({50, -30, -60}, {0, 0, 0}, 90.0);  // on the other side, upside down-ish
    const CameraSolveResult r = solveCamera(pairs, o);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_LT(r.rmsPx, 1e-4);
    EXPECT_LT(distance(r.pose.position, truth.position), 1e-3);
    EXPECT_NEAR(r.focalPx / focal, 1.0, 1e-5);
}

TEST(CameraSolve, FixedFocalNeedsFourPoints) {
    std::mt19937 rng(9);
    const CameraPose truth = aim({20, 15, 60}, {0, 2, 0}, 6.0);
    const double focal = 1.3 * kW;
    const auto pairs = project(truth, focal, headPoints(4, rng));
    const CameraSolveResult r = solveCamera(pairs, options(focal, true));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_LT(r.rmsPx, 1e-4);
    EXPECT_LT(distance(r.pose.position, truth.position), 1e-3);
    EXPECT_DOUBLE_EQ(r.focalPx, focal);  // kept
    // Four pairs are not enough when the focal length is free too.
    const CameraSolveResult free = solveCamera(pairs, options(focal, false));
    EXPECT_FALSE(free.ok);
    EXPECT_NE(free.error.find("at least 6"), std::string::npos) << free.error;
    EXPECT_EQ(cameraSolveMinimumPoints(false), 6);
    EXPECT_EQ(cameraSolveMinimumPoints(true), 4);
}

TEST(CameraSolve, CoplanarPointsFixTheFocalOrSayWhy) {
    std::mt19937 rng(21);
    const CameraPose truth = aim({25, 10, 50}, {0, 0, 0}, 3.0);
    const double focal = 1.5 * kW;
    // A flat card of points (z = 0): the pose is solvable with the focal length known ...
    std::vector<CameraVec3> flat;
    for (int i = 0; i < 8; ++i) flat.push_back({-8.0 + 16.0 * ((i * 37) % 11) / 10.0, -8.0 + 16.0 * ((i * 53) % 7) / 6.0, 0.0});
    const auto pairs = project(truth, focal, flat);
    const CameraSolveResult fixed = solveCamera(pairs, options(focal, true));
    ASSERT_TRUE(fixed.ok) << fixed.error;
    EXPECT_LT(fixed.rmsPx, 1e-4);
    EXPECT_LT(distance(fixed.pose.position, truth.position), 1e-3);
    // ... but not with it free: a clear error, not a wrong camera.
    const CameraSolveResult free = solveCamera(pairs, options());
    EXPECT_FALSE(free.ok);
    EXPECT_NE(free.error.find("plane"), std::string::npos) << free.error;
    // A nearly flat face (a tenth as deep as it is wide) still solves.
    const auto shallow = project(truth, focal, headPoints(10, rng, 0.1));
    const CameraSolveResult r = solveCamera(shallow, options());
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_LT(r.rmsPx, 1e-3);
    EXPECT_NEAR(r.focalPx / focal, 1.0, 1e-3);
}

TEST(CameraSolve, DegenerateInputsSayWhy) {
    const CameraPose truth = aim({0, 0, 50}, {0, 0, 0});
    std::vector<CameraVec3> line;
    for (int i = 0; i < 7; ++i) line.push_back({double(i), 2.0 * i, -1.0 * i});
    const CameraSolveResult r = solveCamera(project(truth, 1000.0, line), options(1000.0, true));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("line"), std::string::npos) << r.error;
    std::mt19937 rng(2);
    const auto pairs = project(truth, 1000.0, headPoints(6, rng));
    CameraSolveOptions noSize = options();
    noSize.imageWidth = 0.0;
    EXPECT_FALSE(solveCamera(pairs, noSize).ok);
    EXPECT_FALSE(solveCamera(pairs, options(0.0, true)).ok);  // a fixed focal length of 0
    EXPECT_FALSE(solveCamera({}, options()).ok);
}

TEST(CameraSolve, EulerAnglesAndFocalUnitsRoundTrip) {
    for (const CameraVec3 angles : {CameraVec3{10, -20, 30}, CameraVec3{-75, 40, 170}, CameraVec3{0, 0, 0}}) {
        const CameraVec3 back = eulerXYZDegrees(rotationFromEulerXYZDegrees(angles));
        for (int i = 0; i < 3; ++i) EXPECT_NEAR(back[i], angles[i], 1e-9);
    }
    // Maya's xyz order: X first, then Y, then Z (world = Rz Ry Rx camera).
    const CameraMat3 r = rotationFromEulerXYZDegrees({90, 0, 90});
    EXPECT_NEAR(r[0][1], 0.0, 1e-12);  // camera Y -> world Z after X, then Z leaves it
    EXPECT_NEAR(r[2][1], 1.0, 1e-12);
    // 35 mm on a 36 mm (1.417 in) film back, 1600 px wide.
    const double px = focalMmToPx(35.0, 1600.0, 36.0 / 25.4);
    EXPECT_NEAR(px, 35.0 / 36.0 * 1600.0, 1e-9);
    EXPECT_NEAR(focalPxToMm(px, 1600.0, 36.0 / 25.4), 35.0, 1e-9);
}

}  // namespace
}  // namespace groomlab
