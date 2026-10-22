#include "uv/Project.h"

#include "core/Profile.h"

#include <Eigen/Eigenvalues>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace lila::uv
{
namespace
{
/** Principal axes of the faces' points, area weighted: columns sorted by decreasing variance. */
glm::dmat3 principalAxes(const geo::Mesh& mesh, std::span<const Index> faces, glm::dvec3& center)
{
    double w = 0.0;
    glm::dvec3 c(0.0);
    for (Index f : faces)
    {
        double a = mesh.faceArea(f);
        c += a * mesh.faceCentroid(f);
        w += a;
    }
    center = w > 0.0 ? c / w : glm::dvec3(0.0);
    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    for (Index f : faces)
    {
        double a = mesh.faceArea(f) / std::max<double>(1.0, double(mesh.faceSize(f)));
        mesh.forEachFaceCorner(f, [&](Index cr) {
            glm::dvec3 d = mesh.position(mesh.cornerPoint(cr)) - center;
            Eigen::Vector3d v(d.x, d.y, d.z);
            cov += a * v * v.transpose();
        });
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(cov);
    glm::dmat3 out(1.0);
    for (int i = 0; i < 3; ++i)
    {
        Eigen::Vector3d v = es.eigenvectors().col(2 - i);
        out[i] = glm::normalize(glm::dvec3(v.x(), v.y(), v.z()));
    }
    return out;
}

glm::dvec3 axisVector(ProjectAxis a)
{
    glm::dvec3 v(0.0);
    v[int(a)] = 1.0;
    return v;
}

double wrapAngle(double a)
{
    while (a > glm::pi<double>())
        a -= glm::two_pi<double>();
    while (a < -glm::pi<double>())
        a += glm::two_pi<double>();
    return a;
}
} // namespace

void planeFrame(const glm::dvec3& n, glm::dvec3& u, glm::dvec3& v)
{
    glm::dvec3 up = std::abs(n.y) > 0.99 ? glm::dvec3(0.0, 0.0, n.y > 0.0 ? -1.0 : 1.0) : glm::dvec3(0.0, 1.0, 0.0);
    u = glm::normalize(glm::cross(up, n));
    v = glm::cross(n, u);
}

bool project(geo::Mesh& mesh, std::span<const Index> faceIds, std::string_view uvSet, const ProjectOptions& opts)
{
    LILA_ZONE();

    std::vector<Index> faces = liveFaces(mesh, faceIds);
    if (faces.empty())
        return false;
    std::span<glm::vec2> uv = uvLayer(mesh, uvSet);

    glm::dvec3 center;
    glm::dmat3 axes = principalAxes(mesh, faces, center);
    geo::Bounds box = mesh.faceBounds(faces);
    if (opts.type != Projection::Camera)
        center = box.center();

    switch (opts.type)
    {
    case Projection::Planar:
    {
        glm::dvec3 n;
        if (opts.axis == ProjectAxis::BestFit)
        {
            glm::dvec3 sum(0.0);
            for (Index f : faces)
                sum += mesh.faceAreaVector(f);
            n = glm::length(sum) > 1e-9 * std::max(1.0, glm::length(box.size()) * glm::length(box.size())) ? glm::normalize(sum) : axes[2];
        }
        else
            n = axisVector(opts.axis);
        glm::dvec3 u, v;
        planeFrame(n, u, v);
        for (Index f : faces)
            mesh.forEachFaceCorner(f, [&](Index c) {
                glm::dvec3 d = mesh.position(mesh.cornerPoint(c)) - center;
                uv[c] = glm::vec2(float(glm::dot(d, u)), float(glm::dot(d, v)));
            });
        break;
    }
    case Projection::Box:
    {
        for (Index f : faces)
        {
            glm::dvec3 fn = mesh.faceNormal(f);
            glm::dvec3 a = glm::abs(fn);
            int k = a.x >= a.y && a.x >= a.z ? 0 : a.y >= a.z ? 1 : 2;
            glm::dvec3 n(0.0);
            n[k] = fn[k] >= 0.0 ? 1.0 : -1.0;
            glm::dvec3 u, v;
            planeFrame(n, u, v);
            mesh.forEachFaceCorner(f, [&](Index c) {
                glm::dvec3 d = mesh.position(mesh.cornerPoint(c)) - center;
                uv[c] = glm::vec2(float(glm::dot(d, u)), float(glm::dot(d, v)));
            });
        }
        break;
    }
    case Projection::Cylindrical:
    case Projection::Spherical:
    {
        glm::dvec3 a = opts.axis == ProjectAxis::BestFit ? axes[0] : axisVector(opts.axis);
        if (opts.axis == ProjectAxis::BestFit && opts.type == Projection::Spherical)
            a = glm::dvec3(0.0, 1.0, 0.0);
        glm::dvec3 u0, v0;
        planeFrame(a, u0, v0);
        double radius = 0.0, w = 0.0;
        for (Index f : faces)
            mesh.forEachFaceCorner(f, [&](Index c) {
                glm::dvec3 d = mesh.position(mesh.cornerPoint(c)) - center;
                radius += opts.type == Projection::Cylindrical ? glm::length(d - glm::dot(d, a) * a) : glm::length(d);
                w += 1.0;
            });
        radius = w > 0.0 ? std::max(radius / w, 1e-9) : 1.0;
        auto angleOf = [&](const glm::dvec3& d) { return std::atan2(glm::dot(d, v0), glm::dot(d, u0)); };
        for (Index f : faces)
        {
            glm::dvec3 fc = mesh.faceCentroid(f) - center;
            double base = angleOf(fc);
            mesh.forEachFaceCorner(f, [&](Index c) {
                glm::dvec3 d = mesh.position(mesh.cornerPoint(c)) - center;
                double h = glm::dot(d, a);
                glm::dvec3 radial = d - h * a;
                // Points on the axis have no angle of their own; they take the face's.
                double th = glm::length(radial) > 1e-9 * radius ? base + wrapAngle(angleOf(d) - base) : base;
                double second = h;
                if (opts.type == Projection::Spherical)
                {
                    double len = glm::length(d);
                    second = len > 0.0 ? std::asin(std::clamp(h / len, -1.0, 1.0)) * radius : 0.0;
                }
                uv[c] = glm::vec2(float(th * radius), float(second));
            });
        }
        break;
    }
    case Projection::Camera:
    {
        for (Index f : faces)
            mesh.forEachFaceCorner(f, [&](Index c) {
                glm::dvec4 clip = opts.camera * glm::dvec4(mesh.position(mesh.cornerPoint(c)), 1.0);
                double iw = std::abs(clip.w) > 1e-12 ? 1.0 / clip.w : 1.0;
                uv[c] = glm::vec2(float(clip.x * iw * 0.5 + 0.5), float(clip.y * iw * 0.5 + 0.5));
            });
        break;
    }
    default: return false;
    }

    if (opts.normalize && opts.type != Projection::Camera)
    {
        glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        for (Index f : faces)
            mesh.forEachFaceCorner(f, [&](Index c) {
                lo = glm::min(lo, uv[c]);
                hi = glm::max(hi, uv[c]);
            });
        float ext = std::max(hi.x - lo.x, hi.y - lo.y);
        float s = ext > 0.0f ? 1.0f / ext : 1.0f;
        for (Index f : faces)
            mesh.forEachFaceCorner(f, [&](Index c) { uv[c] = (uv[c] - lo) * s; });
    }
    if (opts.markSeams)
        markSeamsFromUvs(mesh, faces, uvSet);
    return true;
}
} // namespace lila::uv
