#include "scene/AutoPose.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>

namespace lila::scene::autopose
{
namespace
{
constexpr int kPelvis = 0, kThighL = 8, kCalfL = 9, kFootL = 10, kThighR = 15, kCalfR = 16, kFootR = 17;

/** The body frame training used: pelvis origin, X toward the left hip (level), Y up, in leg lengths. */
struct Frame
{
    glm::dvec3 origin{0.0};
    glm::dmat3 axes{1.0}; // columns: body X, Y, Z in world
    double scale = 1.0;

    glm::dvec3 toBody(const glm::dvec3& p) const { return glm::transpose(axes) * (p - origin) / scale; }
    glm::dvec3 toWorld(const glm::dvec3& p) const { return origin + axes * (p * scale); }
};

Frame frameOf(const Points& pts)
{
    Frame f;
    f.origin = pts[kPelvis];
    glm::dvec3 left = pts[kThighL] - pts[kThighR];
    left.y = 0.0;
    const glm::dvec3 x = glm::length(left) > 1e-6 ? glm::normalize(left) : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 y(0.0, 1.0, 0.0);
    f.axes = glm::dmat3(x, y, glm::cross(x, y));
    const double leg = glm::length(pts[kThighL] - pts[kCalfL]) + glm::length(pts[kCalfL] - pts[kFootL]) +
                       glm::length(pts[kThighR] - pts[kCalfR]) + glm::length(pts[kCalfR] - pts[kFootR]);
    f.scale = std::max(leg / 2.0, 1.0);
    return f;
}

template <class T> bool readValue(std::ifstream& in, T& v) { return bool(in.read(reinterpret_cast<char*>(&v), sizeof v)); }
} // namespace

std::unique_ptr<Model> Model::load(const std::filesystem::path& file, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error)
            *error = paths::toUtf8(file) + ": " + why;
        return nullptr;
    };
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return fail("cannot be opened");
    char magic[4] = {};
    uint32_t version = 0, layers = 0, points = 0;
    if (!in.read(magic, 4) || std::memcmp(magic, "LAPN", 4) != 0 || !readValue(in, version) || !readValue(in, layers) || !readValue(in, points))
        return fail("not an auto-pose model");
    if (version != 1 || points != uint32_t(kPointCount) || layers == 0 || layers > 64)
        return fail("unsupported model version or layout");
    auto model = std::unique_ptr<Model>(new Model);
    int expected = kPointCount * 7;
    for (uint32_t i = 0; i < layers; ++i)
    {
        uint32_t rows = 0, cols = 0, activation = 0;
        if (!readValue(in, rows) || !readValue(in, cols) || !readValue(in, activation) || int(cols) != expected || rows == 0 || rows > 65536)
            return fail("layer " + std::to_string(i) + " is malformed");
        Layer l{int(rows), int(cols), activation == 1, std::vector<float>(size_t(rows) * cols), std::vector<float>(rows)};
        if (!in.read(reinterpret_cast<char*>(l.weights.data()), std::streamsize(l.weights.size() * sizeof(float))) ||
            !in.read(reinterpret_cast<char*>(l.bias.data()), std::streamsize(l.bias.size() * sizeof(float))))
            return fail("truncated");
        expected = int(rows);
        model->mLayers.push_back(std::move(l));
    }
    if (expected != kPointCount * 3)
        return fail("the last layer does not give every point");
    return model;
}

Points Model::predict(const Points& current, const Pins& pins) const
{
    LILA_ZONE();

    const Frame f = frameOf(current);
    // Inputs: the current pose, the pins (zero where none) and the pin mask, as training laid them out.
    std::vector<double> x(size_t(kPointCount) * 7, 0.0);
    Points body;
    for (int i = 0; i < kPointCount; ++i)
    {
        body[size_t(i)] = f.toBody(current[size_t(i)]);
        for (int a = 0; a < 3; ++a)
            x[size_t(i * 3 + a)] = body[size_t(i)][a];
        if (pins[size_t(i)])
        {
            const glm::dvec3 p = f.toBody(*pins[size_t(i)]);
            for (int a = 0; a < 3; ++a)
                x[size_t(kPointCount * 3 + i * 3 + a)] = p[a];
            x[size_t(kPointCount * 6 + i)] = 1.0;
        }
    }
    std::vector<double> y;
    for (const Layer& l : mLayers)
    {
        y.assign(size_t(l.rows), 0.0);
        for (int r = 0; r < l.rows; ++r)
        {
            const float* w = l.weights.data() + size_t(r) * size_t(l.cols);
            double s = l.bias[size_t(r)];
            for (int c = 0; c < l.cols; ++c)
                s += double(w[c]) * x[size_t(c)];
            y[size_t(r)] = l.silu ? s / (1.0 + std::exp(-s)) : s;
        }
        x.swap(y);
    }
    Points out;
    for (int i = 0; i < kPointCount; ++i)
        out[size_t(i)] = f.toWorld(body[size_t(i)] + glm::dvec3(x[size_t(i * 3)], x[size_t(i * 3 + 1)], x[size_t(i * 3 + 2)]));
    return out;
}

const Model* shipped()
{
    static std::once_flag once;
    static std::unique_ptr<Model> model;
    std::call_once(once, [] {
        std::string error;
        model = Model::load(paths::resourcesDir() / "models" / "autopose.lilanet", &error);
        if (!model)
            log::write(LogLevel::Info, "auto-pose: " + error);
    });
    return model.get();
}
} // namespace lila::scene::autopose
