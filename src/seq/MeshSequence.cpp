#include "seq/MeshSequence.h"

#include "core/Profile.h"
#include "core/Progress.h"
#include "fbx/Fbx.h"
#include "geo/ColorSets.h"
#include "geo/ObjIO.h"
#include "geo/Ops.h"
#include "gn/Geometry.h"
#include "math/MathCore.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "seq/Alembic.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace lila::seq
{
namespace
{
double fpsOf(const SequenceOptions& o) { return o.fps > 0.0 ? o.fps : scene::anim::playback().fps; }

std::string padded(double frame, int padding)
{
    if (std::abs(frame - std::round(frame)) < 1e-6)
    {
        int64_t n = std::llround(frame);
        std::string digits = std::to_string(n < 0 ? -n : n);
        if (int(digits.size()) < padding)
            digits.insert(0, size_t(padding) - digits.size(), '0');
        return (n < 0 ? "-" : "") + digits;
    }
    std::string s = std::format("{:.3f}", frame);
    for (char& c : s)
        if (c == '.')
            c = '_';
    return s;
}

std::string replaceAll(std::string s, std::string_view token, const std::string& with)
{
    for (size_t at = s.find(token); at != std::string::npos; at = s.find(token, at + with.size()))
        s.replace(at, token.size(), with);
    return s;
}

std::string fileSafe(std::string s)
{
    for (char& c : s)
        if (c == ':' || c == '|' || c == '/' || c == '\\')
            c = '_';
    return s;
}

std::string baseName(scene::Graph& g, const std::vector<scene::NodeId>& meshes)
{
    const scene::Node* n = meshes.empty() ? nullptr : g.node(meshes.front());
    return fileSafe(n ? n->name() : std::string("mesh"));
}

/** Lila space to file space for Alembic and OBJ: what Unreal's Maya conversion turns into the FBX profile's result. */
glm::dmat3 yUpBasis(const std::string& profile)
{
    const math::AxisBasis& b = math::axisBasis(math::upAxis());
    if (profile == "ue5")
        return glm::transpose(glm::dmat3(b.forward, b.up, b.right));
    if (profile == "ue6")
        return glm::transpose(glm::dmat3(-b.right, b.up, b.forward));
    return glm::dmat3(1.0);
}

void stripLayers(geo::Mesh& m, const SequenceOptions& o)
{
    if (!o.uvs)
        for (int set = 0; set < 8; ++set)
            m.cornerAttrs().remove(geo::attr::uvName(set));
    if (!o.colors)
        for (const std::string& set : geo::colors::sets(m))
            geo::colors::remove(m, set);
    if (!o.normals)
        m.cornerAttrs().remove(geo::attr::kNormal);
}

void scaleMesh(geo::Mesh& m, const glm::dvec3& offset, double scale)
{
    if (offset == glm::dvec3(0.0) && scale == 1.0)
        return;
    glm::dmat4 xf = glm::scale(glm::dmat4(1.0), glm::dvec3(scale)) * glm::translate(glm::dmat4(1.0), -offset);
    gn::transformMesh(m, xf);
}

fs::path filePath(const SequenceOptions& o, const std::string& base, int index, double frame)
{
    std::string stem = frameName(o.pattern, base, index, frame, o.padding);
    return fs::path(o.folder) / (stem + "." + o.format);
}

void writeFrame(const FrameMeshes& f, const fs::path& path, const SequenceOptions& o)
{
    LILA_ZONE();

    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    if (o.format == "obj")
    {
        const glm::dmat4 basis(yUpBasis(o.profile));
        std::vector<geo::ObjObject> objects;
        for (size_t i = 0; i < f.meshes.size(); ++i)
        {
            geo::ObjObject obj{f.names[i], f.meshes[i], {}};
            if (basis != glm::dmat4(1.0))
                gn::transformMesh(obj.mesh, basis);
            if (o.triangulate)
            {
                std::vector<geo::Index> faces;
                for (geo::Index fc = 0; fc < obj.mesh.faceCapacity(); ++fc)
                    if (obj.mesh.faceValid(fc) && obj.mesh.faceSize(fc) > 3)
                        faces.push_back(fc);
                geo::triangulateFaces(obj.mesh, faces);
            }
            objects.push_back(std::move(obj));
        }
        geo::ObjWriteOptions wo;
        wo.normals = o.normals;
        wo.uvs = o.uvs;
        std::ofstream out(path, std::ios::binary);
        out << geo::writeObj(objects, wo);
        if (!out)
            throw std::runtime_error("can't write " + path.string());
        return;
    }
    fbx::ExportScene es;
    es.upAxis = math::upAxis();
    for (size_t i = 0; i < f.meshes.size(); ++i)
    {
        fbx::ExportNode node;
        node.name = f.names[i];
        node.mesh = std::make_shared<const geo::Mesh>(f.meshes[i]);
        es.nodes.push_back(std::move(node));
    }
    fbx::ExportOptions fo;
    if (!fbx::parseProfile(o.profile, &fo.profile))
        throw std::invalid_argument("profile must be ue5, ue6 or generic");
    fo.triangulate = o.triangulate;
    fo.normals = o.normals;
    fo.tangents = o.normals && o.uvs;
    std::string err;
    if (!fbx::writeFbx(es, path, fo, &err))
        throw std::runtime_error(err);
}
} // namespace

std::vector<double> frames(const SequenceOptions& o)
{
    double start = o.start, end = o.end;
    if (o.playbackRange)
    {
        const scene::anim::Playback& pb = scene::anim::playback();
        start = std::round(pb.minTime * fpsOf(o) * 1000.0) / 1000.0;
        end = std::round(pb.maxTime * fpsOf(o) * 1000.0) / 1000.0;
    }
    if (o.step <= 0.0)
        throw std::invalid_argument("step must be positive");
    if (end < start)
        throw std::invalid_argument("end frame is before the start frame");
    std::vector<double> out;
    for (int i = 0;; ++i)
    {
        double f = start + o.step * i;
        if (f > end + 1e-6)
            break;
        out.push_back(f);
    }
    return out;
}

std::string frameName(const std::string& pattern, const std::string& name, int index, double frame, int padding)
{
    std::string s = replaceAll(pattern.empty() ? std::string("{name}_{index}") : pattern, "{name}", name);
    s = replaceAll(std::move(s), "{index}", padded(double(index), padding));
    return replaceAll(std::move(s), "{frame}", padded(frame, padding));
}

bool parseRecenter(std::string_view s, Recenter* out)
{
    if (s == "none" || s == "off")
        *out = Recenter::None;
    else if (s == "first")
        *out = Recenter::First;
    else if (s == "each")
        *out = Recenter::Each;
    else
        return false;
    return true;
}

std::vector<scene::NodeId> meshNodes(scene::Graph& g, const std::vector<scene::NodeId>& roots)
{
    std::vector<scene::NodeId> out;
    auto isMesh = [&](scene::NodeId id) {
        const scene::Node* n = g.node(id);
        return n && n->type().isA("mesh") && !n->type().isA("lattice");
    };
    auto add = [&](scene::NodeId id) {
        if (std::ranges::find(out, id) == out.end())
            out.push_back(id);
    };
    for (scene::NodeId r : roots)
    {
        if (isMesh(r))
        {
            add(r);
            continue;
        }
        for (scene::NodeId d : g.descendants(r))
            if (isMesh(d))
                add(d);
    }
    return out;
}

void sample(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const SequenceOptions& o, const std::function<void(FrameMeshes&)>& sink,
            const std::string& title)
{
    LILA_ZONE();

    if (meshes.empty())
        throw std::invalid_argument("no meshes to export");
    const std::vector<double> list = frames(o);
    const double fps = fpsOf(o);
    const double restore = g.time();
    progress::Scope scope(title, double(list.size()), progress::Cancellable);
    std::optional<glm::dvec3> firstCentre;
    try
    {
        for (size_t i = 0; i < list.size(); ++i)
        {
            scope.checkCancel();
            scope.setStatus(std::format("Frame {}", padded(list[i], 1)));
            g.setTime(list[i] / fps);
            g.evaluate();
            FrameMeshes f;
            f.index = int(i);
            f.frame = list[i];
            geo::Bounds bounds;
            for (scene::NodeId id : meshes)
            {
                scene::Node* n = g.node(id);
                if (!n)
                    continue;
                scene::MeshPtr m = o.applyModifiers ? n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : scene::modifiers::cage(g, id);
                if (!m)
                    continue;
                geo::Mesh copy = *m;
                if (o.worldSpace)
                    gn::transformMesh(copy, n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
                stripLayers(copy, o);
                bounds.extend(copy.bounds());
                // The shortest path, so meshes sharing a name under different parents stay apart in the file.
                f.names.push_back(n->path());
                f.meshes.push_back(std::move(copy));
            }
            glm::dvec3 offset(0.0);
            if (o.recenter == Recenter::Each && bounds.valid())
                offset = bounds.center();
            else if (o.recenter == Recenter::First)
            {
                if (!firstCentre && bounds.valid())
                    firstCentre = bounds.center();
                offset = firstCentre.value_or(glm::dvec3(0.0));
            }
            for (geo::Mesh& m : f.meshes)
                scaleMesh(m, offset, o.scale);
            sink(f);
            scope.step();
        }
    }
    catch (...)
    {
        g.setTime(restore);
        g.evaluate();
        throw;
    }
    g.setTime(restore);
    g.evaluate();
}

std::vector<fs::path> plannedFiles(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const SequenceOptions& o)
{
    const std::string base = baseName(g, meshes);
    if (o.format == "abc")
        return {fs::path(o.folder) / (fileSafe(replaceAll(replaceAll(replaceAll(o.pattern, "_{index}", ""), "_{frame}", ""), "{name}", base)) +
                                      ".abc")};
    std::vector<fs::path> out;
    const std::vector<double> list = frames(o);
    for (size_t i = 0; i < list.size(); ++i)
        out.push_back(filePath(o, base, int(i), list[i]));
    return out;
}

std::vector<fs::path> exportSequence(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const SequenceOptions& o)
{
    LILA_ZONE();

    if (o.format != "obj" && o.format != "fbx" && o.format != "abc")
        throw std::invalid_argument("format must be obj, fbx or abc");
    const std::vector<fs::path> files = plannedFiles(g, meshes, o);
    if (o.format == "abc")
    {
        abc::Writer writer;
        abc::WriteOptions wo;
        wo.fps = fpsOf(o);
        wo.startTime = frames(o).front() / wo.fps;
        wo.normals = o.normals;
        wo.uvs = o.uvs;
        wo.colors = o.colors;
        wo.triangulate = o.triangulate;
        std::error_code ec;
        if (files[0].has_parent_path())
            fs::create_directories(files[0].parent_path(), ec);
        writer.open(files[0], wo);
        const glm::dmat4 basis(yUpBasis(o.profile));
        sample(g, meshes, o, [&](FrameMeshes& f) {
            std::vector<const geo::Mesh*> ptrs;
            for (geo::Mesh& m : f.meshes)
            {
                if (basis != glm::dmat4(1.0))
                    gn::transformMesh(m, basis);
                ptrs.push_back(&m);
            }
            writer.addFrame(f.names, ptrs);
        }, "Exporting Alembic");
        writer.close();
        return files;
    }
    sample(g, meshes, o, [&](FrameMeshes& f) { writeFrame(f, files[size_t(f.index)], o); });
    return files;
}

SequenceOptions fromJson(const nlohmann::json& j)
{
    SequenceOptions o;
    o.start = j.value("start", o.start);
    o.end = j.value("end", o.end);
    o.step = j.value("step", o.step);
    o.playbackRange = j.value("playbackRange", o.playbackRange);
    o.fps = j.value("fps", o.fps);
    o.format = j.value("format", o.format);
    o.folder = j.value("folder", o.folder);
    o.pattern = j.value("pattern", o.pattern);
    o.padding = j.value("padding", o.padding);
    o.scale = j.value("scale", o.scale);
    o.profile = j.value("profile", o.profile);
    o.applyModifiers = j.value("applyModifiers", o.applyModifiers);
    o.worldSpace = j.value("worldSpace", o.worldSpace);
    o.normals = j.value("normals", o.normals);
    o.uvs = j.value("uvs", o.uvs);
    o.colors = j.value("colors", o.colors);
    o.triangulate = j.value("triangulate", o.triangulate);
    if (j.contains("recenter") && j["recenter"].is_string() && !parseRecenter(j["recenter"].get<std::string>(), &o.recenter))
        throw std::invalid_argument("recenter must be none, first or each");
    if (o.format != "obj" && o.format != "fbx" && o.format != "abc")
        throw std::invalid_argument("format must be obj, fbx or abc");
    fbx::AxisProfile p;
    if (!fbx::parseProfile(o.profile, &p))
        throw std::invalid_argument("profile must be ue5, ue6 or generic");
    if (o.padding < 0 || o.padding > 12)
        throw std::invalid_argument("padding must be 0 to 12");
    if (o.scale <= 0.0)
        throw std::invalid_argument("scale must be positive");
    return o;
}

nlohmann::json toJson(const SequenceOptions& o)
{
    const char* recenter = o.recenter == Recenter::First ? "first" : o.recenter == Recenter::Each ? "each" : "none";
    return {{"start", o.start},       {"end", o.end},         {"step", o.step},
            {"playbackRange", o.playbackRange}, {"fps", o.fps}, {"format", o.format},
            {"folder", o.folder},     {"pattern", o.pattern}, {"padding", o.padding},
            {"scale", o.scale},       {"profile", o.profile}, {"applyModifiers", o.applyModifiers},
            {"worldSpace", o.worldSpace}, {"recenter", recenter}, {"normals", o.normals},
            {"uvs", o.uvs},           {"colors", o.colors},   {"triangulate", o.triangulate}};
}
} // namespace lila::seq
