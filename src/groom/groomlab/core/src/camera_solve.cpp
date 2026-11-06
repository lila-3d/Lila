// SPDX-License-Identifier: Apache-2.0
#include "groomlab/camera_solve.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace groomlab {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMmPerInch = 25.4;

using Vec = CameraVec3;
using Mat = CameraMat3;

Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec add(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec scale(const Vec& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const Vec& a) { return std::sqrt(dot(a, a)); }
Vec normalized(const Vec& a) {
    const double n = norm(a);
    return n > 0.0 ? scale(a, 1.0 / n) : Vec{0, 0, 0};
}

Mat mul(const Mat& a, const Mat& b) {
    Mat m{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    return m;
}
Vec mul(const Mat& a, const Vec& v) {
    return {a[0][0] * v[0] + a[0][1] * v[1] + a[0][2] * v[2], a[1][0] * v[0] + a[1][1] * v[1] + a[1][2] * v[2],
            a[2][0] * v[0] + a[2][1] * v[1] + a[2][2] * v[2]};
}
Mat transpose(const Mat& a) {
    Mat m{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = a[j][i];
    return m;
}
double det(const Mat& a) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
}
Mat inverse(const Mat& a) {
    const double d = det(a);
    Mat m{};
    m[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / d;
    m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / d;
    m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / d;
    m[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / d;
    m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / d;
    m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / d;
    m[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / d;
    m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / d;
    m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / d;
    return m;
}

// Rodrigues: the rotation by |w| radians about w.
Mat rotationExp(const Vec& w) {
    const double angle = norm(w);
    Mat r{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    if (angle < 1e-15) return r;
    const Vec k = scale(w, 1.0 / angle);
    const double c = pm::cos(angle), s = pm::sin(angle), t = 1.0 - c;
    r[0] = {c + k[0] * k[0] * t, k[0] * k[1] * t - k[2] * s, k[0] * k[2] * t + k[1] * s};
    r[1] = {k[1] * k[0] * t + k[2] * s, c + k[1] * k[1] * t, k[1] * k[2] * t - k[0] * s};
    r[2] = {k[2] * k[0] * t - k[1] * s, k[2] * k[1] * t + k[0] * s, c + k[2] * k[2] * t};
    return r;
}

// The rotation nearest to m (polar decomposition by Newton iteration); m must have det > 0.
Mat nearestRotation(Mat m) {
    for (int i = 0; i < 50; ++i) {
        const Mat inv = transpose(inverse(m));
        double change = 0.0;
        Mat next{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                next[r][c] = 0.5 * (m[r][c] + inv[r][c]);
                change = std::max(change, std::abs(next[r][c] - m[r][c]));
            }
        m = next;
        if (change < 1e-14) break;
    }
    return m;
}

// Symmetric eigen decomposition (cyclic Jacobi): `a` is n x n row-major; eigenvalues ascending
// in `values`, eigenvectors as the matching columns of `vectors` (n x n row-major).
void symmetricEigen(std::vector<double> a, int n, std::vector<double>& values, std::vector<double>& vectors) {
    std::vector<double> v(static_cast<std::size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i) v[i * n + i] = 1.0;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) off += a[p * n + q] * a[p * n + q];
        if (off < 1e-30) break;
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                const double apq = a[p * n + q];
                if (std::abs(apq) < 1e-300) continue;
                const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < n; ++k) {  // columns p, q
                    const double akp = a[k * n + p], akq = a[k * n + q];
                    a[k * n + p] = c * akp - s * akq;
                    a[k * n + q] = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k) {  // rows p, q
                    const double apk = a[p * n + k], aqk = a[q * n + k];
                    a[p * n + k] = c * apk - s * aqk;
                    a[q * n + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k) {
                    const double vkp = v[k * n + p], vkq = v[k * n + q];
                    v[k * n + p] = c * vkp - s * vkq;
                    v[k * n + q] = s * vkp + c * vkq;
                }
            }
        }
    }
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return a[x * n + x] < a[y * n + y]; });
    values.assign(n, 0.0);
    vectors.assign(static_cast<std::size_t>(n) * n, 0.0);
    for (int j = 0; j < n; ++j) {
        values[j] = a[order[j] * n + order[j]];
        for (int k = 0; k < n; ++k) vectors[k * n + j] = v[k * n + order[j]];
    }
}

// Solves the n x n system a x = b in place (Gaussian elimination with partial pivoting).
bool solveLinear(std::vector<double> a, std::vector<double> b, int n, std::vector<double>& x) {
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int r = col + 1; r < n; ++r)
            if (std::abs(a[r * n + col]) > std::abs(a[pivot * n + col])) pivot = r;
        if (std::abs(a[pivot * n + col]) < 1e-300) return false;
        if (pivot != col) {
            for (int k = 0; k < n; ++k) std::swap(a[col * n + k], a[pivot * n + k]);
            std::swap(b[col], b[pivot]);
        }
        for (int r = col + 1; r < n; ++r) {
            const double f = a[r * n + col] / a[col * n + col];
            for (int k = col; k < n; ++k) a[r * n + k] -= f * a[col * n + k];
            b[r] -= f * b[col];
        }
    }
    x.assign(n, 0.0);
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int k = r + 1; k < n; ++k) s -= a[r * n + k] * x[k];
        x[r] = s / a[r * n + r];
    }
    return true;
}

// The camera as the solver works on it: computer-vision axes (x right, y down, z forward),
// world -> camera rotation `r` and translation `t`, focal length in pixels.
struct CvCamera {
    Mat r{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    Vec t{0, 0, 0};
    double focal = 1.0;
};

// Maya's axes (camera Y up, looking down -Z) are the CV ones with y and z flipped.
CvCamera fromPose(const CameraPose& pose, double focal) {
    const Mat cw = pose.rotation;  // columns: camera X, Y, Z in world
    CvCamera c;
    for (int j = 0; j < 3; ++j) {
        c.r[0][j] = cw[j][0];
        c.r[1][j] = -cw[j][1];
        c.r[2][j] = -cw[j][2];
    }
    c.t = scale(mul(c.r, pose.position), -1.0);
    c.focal = focal;
    return c;
}

CameraPose toPose(const CvCamera& c) {
    CameraPose pose;
    for (int j = 0; j < 3; ++j) {
        pose.rotation[j][0] = c.r[0][j];
        pose.rotation[j][1] = -c.r[1][j];
        pose.rotation[j][2] = -c.r[2][j];
    }
    pose.position = scale(mul(transpose(c.r), c.t), -1.0);
    return pose;
}

struct Problem {
    std::vector<CameraCorrespondence> points;
    double cx = 0.0, cy = 0.0;
    double behindPenalty = 1e4;  // a residual for a point behind the camera
};

// Residuals (du, dv per point), pixels.
void residuals(const Problem& p, const CvCamera& c, std::vector<double>& out) {
    out.resize(p.points.size() * 2);
    for (std::size_t i = 0; i < p.points.size(); ++i) {
        const Vec pc = add(mul(c.r, p.points[i].world), c.t);
        if (pc[2] <= 1e-12) {
            out[2 * i] = p.behindPenalty;
            out[2 * i + 1] = p.behindPenalty;
            continue;
        }
        out[2 * i] = p.cx + c.focal * pc[0] / pc[2] - p.points[i].u;
        out[2 * i + 1] = p.cy + c.focal * pc[1] / pc[2] - p.points[i].v;
    }
}

double costOf(const std::vector<double>& r) {
    double s = 0.0;
    for (double x : r) s += x * x;
    return s;
}

bool allInFront(const Problem& p, const CvCamera& c) {
    for (const auto& pt : p.points)
        if (add(mul(c.r, pt.world), c.t)[2] <= 1e-12) return false;
    return true;
}

// The camera moved by parameters d: rotation increment (3), translation (3), log focal (1).
CvCamera applyStep(const CvCamera& c, const std::vector<double>& d, bool fixFocal) {
    CvCamera out = c;
    out.r = mul(rotationExp({d[0], d[1], d[2]}), c.r);
    out.t = {c.t[0] + d[3], c.t[1] + d[4], c.t[2] + d[5]};
    if (!fixFocal) out.focal = c.focal * pm::exp(d[6]);
    return out;
}

// Levenberg-Marquardt from `start`; returns the refined camera and its cost.
CvCamera refine(const Problem& p, CvCamera c, bool fixFocal, int iterations, double& cost) {
    const int n = fixFocal ? 6 : 7;
    const std::size_t m = p.points.size() * 2;
    std::vector<double> r, rp, rm;
    residuals(p, c, r);
    cost = costOf(r);
    double lambda = -1.0;
    std::vector<double> jac(m * n), step(7, 0.0), delta;
    for (int it = 0; it < iterations; ++it) {
        // Numerical Jacobian (central differences).
        const double tScale = std::max(1.0, norm(c.t));
        for (int k = 0; k < n; ++k) {
            const double h = k < 3 ? 1e-6 : (k < 6 ? 1e-6 * tScale : 1e-6);
            std::fill(step.begin(), step.end(), 0.0);
            step[k] = h;
            residuals(p, applyStep(c, step, fixFocal), rp);
            step[k] = -h;
            residuals(p, applyStep(c, step, fixFocal), rm);
            for (std::size_t i = 0; i < m; ++i) jac[i * n + k] = (rp[i] - rm[i]) / (2.0 * h);
        }
        std::vector<double> a(n * n, 0.0), g(n, 0.0);
        for (std::size_t i = 0; i < m; ++i)
            for (int j = 0; j < n; ++j) {
                g[j] += jac[i * n + j] * r[i];
                for (int k = 0; k < n; ++k) a[j * n + k] += jac[i * n + j] * jac[i * n + k];
            }
        if (lambda < 0.0) {
            double maxDiag = 0.0;
            for (int j = 0; j < n; ++j) maxDiag = std::max(maxDiag, a[j * n + j]);
            lambda = 1e-3 * std::max(maxDiag, 1e-12);
        }
        bool improved = false;
        for (int tries = 0; tries < 12 && !improved; ++tries) {
            std::vector<double> damped = a;
            for (int j = 0; j < n; ++j) damped[j * n + j] += lambda * (1.0 + a[j * n + j]);
            std::vector<double> rhs(n);
            for (int j = 0; j < n; ++j) rhs[j] = -g[j];
            if (!solveLinear(damped, rhs, n, delta)) {
                lambda *= 10.0;
                continue;
            }
            std::fill(step.begin(), step.end(), 0.0);
            for (int j = 0; j < n; ++j) step[j] = delta[j];
            const CvCamera next = applyStep(c, step, fixFocal);
            residuals(p, next, rp);
            const double nextCost = costOf(rp);
            if (std::isfinite(nextCost) && nextCost < cost) {
                const double gain = cost - nextCost;
                c = next;
                r = rp;
                cost = nextCost;
                lambda = std::max(lambda / 3.0, 1e-15);
                improved = true;
                double stepSize = 0.0;
                for (double d : delta) stepSize = std::max(stepSize, std::abs(d));
                if (gain < 1e-14 * (1.0 + cost) || stepSize < 1e-13) return c;
            } else {
                lambda *= 4.0;
            }
        }
        if (!improved) break;
    }
    return c;
}

// A camera at `eye` looking at `target`, rolled by `roll` radians about its view axis.
CvCamera lookAt(const Vec& eye, const Vec& target, double roll, double focal) {
    const Vec forward = normalized(sub(target, eye));  // CV +z
    Vec up = {0, 1, 0};
    if (std::abs(dot(up, forward)) > 0.95) up = {0, 0, 1};
    Vec right = normalized(cross(forward, up));  // CV +x (world up -> image up, y down)
    Vec down = cross(forward, right);
    const double c = pm::cos(roll), s = pm::sin(roll);
    const Vec x = add(scale(right, c), scale(down, s));
    const Vec y = add(scale(down, c), scale(right, -s));
    CvCamera cam;
    cam.r = {{{x[0], x[1], x[2]}, {y[0], y[1], y[2]}, {forward[0], forward[1], forward[2]}}};
    cam.t = scale(mul(cam.r, eye), -1.0);
    cam.focal = focal;
    return cam;
}

// First estimate from the Direct Linear Transform (6+ points, not coplanar). False if it fails.
bool dltEstimate(const Problem& p, CvCamera& out) {
    const std::size_t n = p.points.size();
    // Hartley normalization of both point sets.
    double iu = 0, iv = 0;
    Vec wc{0, 0, 0};
    for (const auto& pt : p.points) {
        iu += pt.u - p.cx;
        iv += pt.v - p.cy;
        wc = add(wc, pt.world);
    }
    iu /= n;
    iv /= n;
    wc = scale(wc, 1.0 / n);
    double id = 0, wd = 0;
    for (const auto& pt : p.points) {
        id += pm::hypot(pt.u - p.cx - iu, pt.v - p.cy - iv);
        wd += norm(sub(pt.world, wc));
    }
    id /= n;
    wd /= n;
    if (id <= 0 || wd <= 0) return false;
    const double is = std::sqrt(2.0) / id, ws = std::sqrt(3.0) / wd;
    std::vector<double> ata(144, 0.0);
    for (const auto& pt : p.points) {
        const double x = (pt.u - p.cx - iu) * is, y = (pt.v - p.cy - iv) * is;
        const Vec w = scale(sub(pt.world, wc), ws);
        const double X[4] = {w[0], w[1], w[2], 1.0};
        double row1[12] = {}, row2[12] = {};
        for (int k = 0; k < 4; ++k) {
            row1[k] = X[k];
            row1[8 + k] = -x * X[k];
            row2[4 + k] = X[k];
            row2[8 + k] = -y * X[k];
        }
        for (int a = 0; a < 12; ++a)
            for (int b = 0; b < 12; ++b) ata[a * 12 + b] += row1[a] * row1[b] + row2[a] * row2[b];
    }
    std::vector<double> values, vectors;
    symmetricEigen(ata, 12, values, vectors);
    double pn[3][4];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) pn[r][c] = vectors[(r * 4 + c) * 12 + 0];
    // Undo the normalization: P = Ti^-1 Pn Tw.
    // Tw = [ws*I, -ws*wc; 0 1], Ti^-1 = [1/is 0 iu; 0 1/is iv; 0 0 1].
    double pw[3][4];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) pw[r][c] = pn[r][c] * ws;
        pw[r][3] = pn[r][3] - ws * (pn[r][0] * wc[0] + pn[r][1] * wc[1] + pn[r][2] * wc[2]);
    }
    double P[3][4];
    for (int c = 0; c < 4; ++c) {
        P[0][c] = pw[0][c] / is + iu * pw[2][c];
        P[1][c] = pw[1][c] / is + iv * pw[2][c];
        P[2][c] = pw[2][c];
    }
    // P ~ diag(f, f, 1) [R | t]: the third row's rotation part has unit length.
    const Vec m3 = {P[2][0], P[2][1], P[2][2]};
    double lambda = norm(m3);
    if (lambda <= 0) return false;
    // In front: positive depth for the points' centroid.
    const double depth = P[2][0] * wc[0] + P[2][1] * wc[1] + P[2][2] * wc[2] + P[2][3];
    if (depth < 0) lambda = -lambda;
    const Vec m1 = {P[0][0], P[0][1], P[0][2]}, m2 = {P[1][0], P[1][1], P[1][2]};
    const double focal = 0.5 * (norm(m1) + norm(m2)) / std::abs(lambda);
    if (!(focal > 0) || !std::isfinite(focal)) return false;
    Mat r = {{{m1[0] / (lambda * focal), m1[1] / (lambda * focal), m1[2] / (lambda * focal)},
              {m2[0] / (lambda * focal), m2[1] / (lambda * focal), m2[2] / (lambda * focal)},
              {m3[0] / lambda, m3[1] / lambda, m3[2] / lambda}}};
    if (det(r) <= 0) return false;  // a mirror image: the pairs do not fit a camera
    out.r = nearestRotation(r);
    out.t = {P[0][3] / (lambda * focal), P[1][3] / (lambda * focal), P[2][3] / lambda};
    out.focal = focal;
    return std::isfinite(out.t[0]) && std::isfinite(out.t[1]) && std::isfinite(out.t[2]);
}

// Directions spread over the sphere (a cube's faces, edges and corners).
std::vector<Vec> sphereDirections() {
    std::vector<Vec> dirs;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z)
                if (x || y || z) dirs.push_back(normalized({double(x), double(y), double(z)}));
    return dirs;
}

}  // namespace

int cameraSolveMinimumPoints(bool fixFocal) { return fixFocal ? 4 : 6; }

bool projectToImage(const CameraPose& pose, double focalPx, double imageWidth, double imageHeight,
                    const CameraVec3& world, double& u, double& v) {
    const CvCamera c = fromPose(pose, focalPx);
    const Vec pc = add(mul(c.r, world), c.t);
    if (pc[2] <= 1e-12) return false;
    u = 0.5 * imageWidth + focalPx * pc[0] / pc[2];
    v = 0.5 * imageHeight + focalPx * pc[1] / pc[2];
    return true;
}

double focalPxToMm(double focalPx, double imageWidth, double apertureInches) {
    return imageWidth > 0 ? focalPx / imageWidth * apertureInches * kMmPerInch : 0.0;
}

double focalMmToPx(double focalMm, double imageWidth, double apertureInches) {
    return apertureInches > 0 ? focalMm / (apertureInches * kMmPerInch) * imageWidth : 0.0;
}

CameraVec3 eulerXYZDegrees(const CameraMat3& r) {
    // R = Rz(c) Ry(b) Rx(a)
    const double sb = std::clamp(-r[2][0], -1.0, 1.0);
    const double b = pm::asin(sb);
    double a, c;
    if (std::abs(sb) < 1.0 - 1e-10) {
        a = pm::atan2(r[2][1], r[2][2]);
        c = pm::atan2(r[1][0], r[0][0]);
    } else {  // gimbal lock: all of the turn about x
        c = 0.0;
        a = pm::atan2(-r[1][2], r[1][1]);
    }
    const double k = 180.0 / kPi;
    return {a * k, b * k, c * k};
}

CameraMat3 rotationFromEulerXYZDegrees(const CameraVec3& degrees) {
    const double k = kPi / 180.0;
    const Mat rx = rotationExp({degrees[0] * k, 0, 0});
    const Mat ry = rotationExp({0, degrees[1] * k, 0});
    const Mat rz = rotationExp({0, 0, degrees[2] * k});
    return mul(rz, mul(ry, rx));
}

CameraSolveResult solveCamera(const std::vector<CameraCorrespondence>& points, const CameraSolveOptions& options) {
    CameraSolveResult result;
    const bool fixFocal = options.fixFocal;
    const int minimum = cameraSolveMinimumPoints(fixFocal);
    if (static_cast<int>(points.size()) < minimum) {
        result.error = "needs at least " + std::to_string(minimum) + " point pairs" +
                       (fixFocal ? std::string(" with the focal length fixed") : std::string(" (4 with the focal length fixed)")) +
                       "; got " + std::to_string(points.size());
        return result;
    }
    if (!(options.imageWidth > 0) || !(options.imageHeight > 0)) {
        result.error = "the image size must be positive";
        return result;
    }
    if (fixFocal && !(options.focalPx > 0)) {
        result.error = "a fixed focal length must be positive";
        return result;
    }
    for (const auto& pt : points) {
        if (!std::isfinite(pt.u) || !std::isfinite(pt.v) || !std::isfinite(pt.world[0]) ||
            !std::isfinite(pt.world[1]) || !std::isfinite(pt.world[2])) {
            result.error = "a point is not a finite number";
            return result;
        }
    }

    // The layout of the mesh points: a line never fixes a camera; a plane doesn't fix the focal length.
    const std::size_t n = points.size();
    Vec centroid{0, 0, 0};
    for (const auto& pt : points) centroid = add(centroid, pt.world);
    centroid = scale(centroid, 1.0 / n);
    std::vector<double> cov(9, 0.0);
    for (const auto& pt : points) {
        const Vec d = sub(pt.world, centroid);
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) cov[a * 3 + b] += d[a] * d[b] / n;
    }
    std::vector<double> spread, axes;
    symmetricEigen(cov, 3, spread, axes);
    const double extent = std::sqrt(std::max(spread[2], 0.0));
    if (!(extent > 0)) {
        result.error = "the points on the mesh are all the same point";
        return result;
    }
    if (std::sqrt(std::max(spread[1], 0.0)) < 1e-4 * extent) {
        result.error = "the points on the mesh lie on a line: pick points spread over the mesh";
        return result;
    }
    const bool planar = std::sqrt(std::max(spread[0], 0.0)) < 0.02 * extent;
    if (planar && !fixFocal) {
        result.error = "the points on the mesh lie on one plane, which cannot fix the focal length: pick points "
                       "at different depths, or fix the focal length";
        return result;
    }
    double imageSpread = 0.0;
    for (const auto& pt : points) imageSpread = std::max(imageSpread, pm::hypot(pt.u - points[0].u, pt.v - points[0].v));
    if (imageSpread < 1.0) {
        result.error = "the points on the image are all the same point";
        return result;
    }

    Problem problem;
    problem.points = points;
    problem.cx = 0.5 * options.imageWidth;
    problem.cy = 0.5 * options.imageHeight;
    problem.behindPenalty = 10.0 * (options.imageWidth + options.imageHeight);

    // Starting cameras: the DLT estimate, the caller's guess, and views from all around.
    std::vector<CvCamera> starts;
    if (!fixFocal) {
        CvCamera dlt;
        if (dltEstimate(problem, dlt)) starts.push_back(dlt);
    }
    const double width = options.imageWidth;
    std::vector<double> focals;
    if (options.focalPx > 0) focals.push_back(options.focalPx);
    if (!fixFocal) {
        for (double f : {0.8 * width, 1.8 * width, 4.0 * width})
            if (focals.empty() || std::abs(f - focals[0]) > 0.2 * focals[0]) focals.push_back(f);
    }
    if (options.hasInitialPose) {
        for (double f : focals) starts.push_back(fromPose(options.initialPose, f));
    }
    // The points fill about half the image from the starting distance.
    double radius = 0.0;
    for (const auto& pt : points) radius = std::max(radius, norm(sub(pt.world, centroid)));
    for (const Vec& dir : sphereDirections()) {
        for (double f : focals) {
            const double distance = std::max(radius * f / (0.35 * width), 2.0 * radius);
            for (int roll = 0; roll < 4; ++roll)
                starts.push_back(lookAt(add(centroid, scale(dir, distance)), centroid, roll * 0.5 * kPi, f));
        }
    }

    // A short run from every start, then the best few to convergence.
    std::vector<std::pair<double, CvCamera>> tried;
    tried.reserve(starts.size());
    for (const CvCamera& s : starts) {
        double cost = 0.0;
        const CvCamera c = refine(problem, s, fixFocal, 25, cost);
        if (std::isfinite(cost)) tried.emplace_back(cost, c);
    }
    std::stable_sort(tried.begin(), tried.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    CvCamera best;
    double bestCost = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < std::min<std::size_t>(tried.size(), 6); ++i) {
        double cost = 0.0;
        const CvCamera c = refine(problem, tried[i].second, fixFocal, options.maxIterations, cost);
        if (cost < bestCost && allInFront(problem, c) && c.focal > 0) {
            bestCost = cost;
            best = c;
        }
    }
    if (!std::isfinite(bestCost)) {
        result.error = "no camera sees every point in front of it: check that the pairs match";
        return result;
    }

    std::vector<double> r;
    residuals(problem, best, r);
    result.errorsPx.resize(n);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        result.errorsPx[i] = pm::hypot(r[2 * i], r[2 * i + 1]);
        sum += result.errorsPx[i] * result.errorsPx[i];
    }
    result.rmsPx = std::sqrt(sum / n);
    result.pose = toPose(best);
    result.focalPx = best.focal;
    result.ok = true;
    return result;
}

}  // namespace groomlab
