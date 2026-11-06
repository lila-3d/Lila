// SPDX-License-Identifier: Apache-2.0
#pragma once

// Lining a camera up with a photo: a pinhole camera (rotation, position, focal length) solved
// from points picked on the image and the same points on the mesh (2D-3D correspondences).
//
// The camera model is Maya's: the camera looks down its -Z axis with +Y up and +X to the
// right; the principal point is the image's center, pixels are square, there is no lens
// distortion. Image points are in pixels with the origin at the image's top-left corner, u to
// the right and v down (as image editors and Qt count them).
//
// With a free focal length the solve needs 6 points or more, not all on one plane (a Direct
// Linear Transform gives the first estimate); with the focal length fixed, 4 points are enough
// (any layout but a line). Levenberg-Marquardt then refines the pose (and the focal length)
// to the smallest reprojection error, from the DLT estimate, the caller's guess and a spread
// of starting views around the points, so a bad first guess does not matter.

#include <array>
#include <string>
#include <vector>

namespace groomlab {

using CameraVec3 = std::array<double, 3>;
using CameraMat3 = std::array<std::array<double, 3>, 3>;  // [row][column]

// A camera's pose: `rotation` takes camera axes to world axes (its columns are the camera's
// X, Y and Z axes in world space; the camera looks down -Z), `position` is the eye.
struct CameraPose {
    CameraMat3 rotation{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    CameraVec3 position{0, 0, 0};
};

struct CameraCorrespondence {
    double u = 0.0, v = 0.0;  // image pixels, origin top-left, v down
    CameraVec3 world{0, 0, 0};
};

struct CameraSolveOptions {
    double imageWidth = 0.0;   // pixels
    double imageHeight = 0.0;  // pixels
    double focalPx = 0.0;      // the focal length in pixels: the guess, or the fixed value (0: unknown)
    bool fixFocal = false;     // solve the pose only, keeping focalPx (needs focalPx > 0)
    bool hasInitialPose = false;
    CameraPose initialPose;    // a starting guess (e.g. where the camera is now)
    int maxIterations = 200;   // per Levenberg-Marquardt run
};

struct CameraSolveResult {
    bool ok = false;
    std::string error;          // why not, when !ok
    CameraPose pose;
    double focalPx = 0.0;
    double rmsPx = 0.0;         // root mean square reprojection error, pixels
    std::vector<double> errorsPx;  // per correspondence, pixels
};

// Minimum correspondences: 6 with a free focal length, 4 with it fixed.
int cameraSolveMinimumPoints(bool fixFocal);

CameraSolveResult solveCamera(const std::vector<CameraCorrespondence>& points, const CameraSolveOptions& options);

// Projects a world point with the camera: pixels (origin top-left, v down). False when the
// point is not in front of the camera.
bool projectToImage(const CameraPose& pose, double focalPx, double imageWidth, double imageHeight,
                    const CameraVec3& world, double& u, double& v);

// Focal length in pixels <-> millimeters, for an image `imageWidth` pixels wide spanning a film
// back `apertureInches` wide (Maya's horizontal film aperture).
double focalPxToMm(double focalPx, double imageWidth, double apertureInches);
double focalMmToPx(double focalMm, double imageWidth, double apertureInches);

// Euler angles (degrees) in Maya's xyz rotation order for a rotation matrix (as above: world =
// R * camera, R = Rz * Ry * Rx for xyz order acting on column vectors), and back.
CameraVec3 eulerXYZDegrees(const CameraMat3& rotation);
CameraMat3 rotationFromEulerXYZDegrees(const CameraVec3& degrees);

}  // namespace groomlab
