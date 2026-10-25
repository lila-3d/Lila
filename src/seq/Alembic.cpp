#include "seq/Alembic.h"

#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/Normals.h"
#include "geo/Triangulate.h"

#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>

namespace lila::seq::abc
{
namespace AbcG = Alembic::AbcGeom;
using geo::Index;

namespace
{
/** Alembic object names cannot hold '/'; namespaces' ':' are kept out too. */
std::string objectName(const std::string& name)
{
    std::string out = name.empty() ? std::string("mesh") : name;
    for (char& c : out)
        if (c == '/' || c == ':' || c == '|')
            c = '_';
    return out;
}

/** Deduplicated facevarying values plus an index per written corner. */
template <class T> struct Indexed
{
    std::vector<T> values;
    std::vector<uint32_t> index;
};

template <class T, class Key> Indexed<T> indexCorners(const std::vector<Index>& corners, std::span<const Key> src)
{
    Indexed<T> out;
    std::map<std::array<float, sizeof(Key) / sizeof(float)>, uint32_t> ids;
    out.index.reserve(corners.size());
    for (Index c : corners)
    {
        std::array<float, sizeof(Key) / sizeof(float)> k;
        std::memcpy(k.data(), &src[c], sizeof(Key));
        auto [it, added] = ids.emplace(k, uint32_t(out.values.size()));
        if (added)
        {
            T v;
            std::memcpy(&v, &src[c], sizeof(Key));
            out.values.push_back(v);
        }
        out.index.push_back(it->second);
    }
    return out;
}

struct Object
{
    AbcG::OXform xform;
    AbcG::OPolyMesh mesh;
    std::vector<AbcG::OV2fGeomParam> extraUvs;
    AbcG::OC4fGeomParam color;
    bool hasColor = false;
    int written = 0;
};
} // namespace

struct Writer::Impl
{
    WriteOptions opts;
    AbcG::OArchive archive;
    std::map<std::string, Object> objects;
    std::vector<std::string> order;
    int frames = 0;

    void write(Object& o, const geo::Mesh* m);
};

Writer::Writer() : mImpl(std::make_unique<Impl>()) {}
Writer::~Writer() { close(); }

void Writer::open(const fs::path& path, const WriteOptions& opts)
{
    LILA_ZONE();

    close();
    mImpl = std::make_unique<Impl>();
    mImpl->opts = opts;
    try
    {
        mImpl->archive = AbcG::OArchive(Alembic::AbcCoreOgawa::WriteArchive(), path.string(), Alembic::Abc::MetaData(),
                                        Alembic::Abc::ErrorHandler::kThrowPolicy);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error("can't create " + path.string() + ": " + e.what());
    }
    mImpl->archive.setCompressionHint(1);
}

int Writer::frameCount() const { return mImpl ? mImpl->frames : 0; }

void Writer::Impl::write(Object& o, const geo::Mesh* m)
{
    LILA_ZONE();

    AbcG::OPolyMeshSchema& schema = o.mesh.getSchema();
    if (!m || m->numFaces() == 0)
    {
        // Keeps the samples uniform while the object is absent: an empty mesh for the frame.
        std::vector<Imath::V3f> none;
        std::vector<int32_t> noIdx;
        schema.set(AbcG::OPolyMeshSchema::Sample(AbcG::P3fArraySample(none), AbcG::Int32ArraySample(noIdx), AbcG::Int32ArraySample(noIdx)));
        std::vector<Imath::C4f> noColor;
        std::vector<uint32_t> noColorIdx;
        if (o.hasColor)
            o.color.set(AbcG::OC4fGeomParam::Sample(AbcG::C4fArraySample(noColor), AbcG::UInt32ArraySample(noColorIdx), AbcG::kFacevaryingScope));
        ++o.written;
        return;
    }

    std::vector<Imath::V3f> points;
    std::vector<int32_t> pointIndex(m->pointCapacity(), -1);
    points.reserve(m->numPoints());
    for (Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p))
        {
            pointIndex[p] = int32_t(points.size());
            const glm::dvec3& v = m->position(p);
            points.emplace_back(float(v.x), float(v.y), float(v.z));
        }

    // Corners per written face, reversed to clockwise.
    std::vector<Index> corners;
    std::vector<int32_t> counts;
    if (opts.triangulate)
    {
        geo::Triangulation tris = geo::triangulate(*m);
        corners.reserve(tris.corners.size());
        for (Index t = 0; t < tris.triangleCount(); ++t)
        {
            corners.insert(corners.end(), {tris.corners[t * 3 + 2], tris.corners[t * 3 + 1], tris.corners[t * 3]});
            counts.push_back(3);
        }
    }
    else
    {
        for (Index f = 0; f < m->faceCapacity(); ++f)
        {
            if (!m->faceValid(f))
                continue;
            std::vector<Index> fc = m->faceCorners(f);
            corners.insert(corners.end(), fc.rbegin(), fc.rend());
            counts.push_back(int32_t(fc.size()));
        }
    }
    std::vector<int32_t> indices;
    indices.reserve(corners.size());
    for (Index c : corners)
        indices.push_back(pointIndex[m->cornerPoint(c)]);

    AbcG::OV2fGeomParam::Sample uvSample;
    Indexed<Imath::V2f> uv0;
    std::vector<Indexed<Imath::V2f>> extra;
    if (opts.uvs)
    {
        for (int set = 0; set < 8; ++set)
        {
            std::span<const glm::vec2> uv = m->cornerAttrs().get<glm::vec2>(geo::attr::uvName(set));
            if (uv.size() != m->cornerCapacity())
                continue;
            Indexed<Imath::V2f> ix = indexCorners<Imath::V2f>(corners, uv);
            if (set == 0)
                uv0 = std::move(ix);
            else
                extra.push_back(std::move(ix));
        }
        if (!uv0.values.empty())
            uvSample = AbcG::OV2fGeomParam::Sample(AbcG::V2fArraySample(uv0.values), AbcG::UInt32ArraySample(uv0.index), AbcG::kFacevaryingScope);
    }

    AbcG::ON3fGeomParam::Sample nSample;
    Indexed<Imath::V3f> normals;
    if (opts.normals)
    {
        std::vector<glm::vec3> computed;
        std::span<const glm::vec3> n = m->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
        if (n.size() != m->cornerCapacity())
        {
            computed = geo::cornerNormals(*m);
            n = computed;
        }
        normals = indexCorners<Imath::V3f>(corners, n);
        nSample = AbcG::ON3fGeomParam::Sample(AbcG::N3fArraySample(normals.values), AbcG::UInt32ArraySample(normals.index), AbcG::kFacevaryingScope);
    }

    AbcG::OPolyMeshSchema::Sample sample(AbcG::P3fArraySample(points), AbcG::Int32ArraySample(indices), AbcG::Int32ArraySample(counts), uvSample,
                                         nSample);
    schema.set(sample);

    for (size_t i = 0; i < extra.size(); ++i)
    {
        if (i >= o.extraUvs.size())
        {
            if (o.written > 0)
                continue; // a set that appears mid-sequence would start late; keep the layout of the first frame
            o.extraUvs.emplace_back(schema.getArbGeomParams(), "uv" + std::to_string(i + 1), true, AbcG::kFacevaryingScope, 1,
                                    schema.getTimeSampling());
        }
        o.extraUvs[i].set(AbcG::OV2fGeomParam::Sample(AbcG::V2fArraySample(extra[i].values), AbcG::UInt32ArraySample(extra[i].index),
                                                      AbcG::kFacevaryingScope));
    }

    std::vector<std::string> sets = opts.colors ? geo::colors::sets(*m) : std::vector<std::string>{};
    std::span<const glm::vec4> col = sets.empty() ? std::span<const glm::vec4>{} : geo::colors::values(*m, sets.front());
    if (col.size() == m->cornerCapacity() && !col.empty())
    {
        if (!o.hasColor && o.written == 0)
        {
            o.color = AbcG::OC4fGeomParam(schema.getArbGeomParams(), "Cd", true, AbcG::kFacevaryingScope, 1, schema.getTimeSampling());
            o.hasColor = true;
        }
        if (o.hasColor)
        {
            Indexed<Imath::C4f> c = indexCorners<Imath::C4f>(corners, col);
            o.color.set(AbcG::OC4fGeomParam::Sample(AbcG::C4fArraySample(c.values), AbcG::UInt32ArraySample(c.index), AbcG::kFacevaryingScope));
        }
    }
    else if (o.hasColor)
    {
        std::vector<Imath::C4f> white(1, Imath::C4f(1.0f, 1.0f, 1.0f, 1.0f));
        std::vector<uint32_t> zero(corners.size(), 0u);
        o.color.set(AbcG::OC4fGeomParam::Sample(AbcG::C4fArraySample(white), AbcG::UInt32ArraySample(zero), AbcG::kFacevaryingScope));
    }
    ++o.written;
}

void Writer::addFrame(const std::vector<std::string>& names, const std::vector<const geo::Mesh*>& meshes)
{
    LILA_ZONE();

    if (!mImpl || !mImpl->archive.valid())
        throw std::runtime_error("Alembic writer is not open");
    Impl& im = *mImpl;
    const double spf = 1.0 / im.opts.fps;
    std::map<std::string, const geo::Mesh*> now;
    for (size_t i = 0; i < names.size() && i < meshes.size(); ++i)
        now[objectName(names[i])] = meshes[i];

    for (auto& [name, mesh] : now)
    {
        if (im.objects.contains(name) || !mesh || mesh->numFaces() == 0)
            continue;
        // Each object's samples start at its first frame.
        uint32_t ts = im.archive.addTimeSampling(AbcG::TimeSampling(spf, im.opts.startTime + im.frames * spf));
        Object o;
        o.xform = AbcG::OXform(im.archive.getTop(), name, ts);
        AbcG::XformSample identity;
        o.xform.getSchema().set(identity);
        o.mesh = AbcG::OPolyMesh(o.xform, name + "Shape", ts);
        im.objects.emplace(name, std::move(o));
        im.order.push_back(name);
    }
    for (const std::string& name : im.order)
    {
        Object& o = im.objects[name];
        auto it = now.find(name);
        AbcG::XformSample identity;
        if (o.written > 0)
            o.xform.getSchema().set(identity);
        im.write(o, it == now.end() ? nullptr : it->second);
    }
    ++im.frames;
}

void Writer::addTransformFrame(const std::vector<std::string>& names, const std::vector<const geo::Mesh*>& shapes,
                               const std::vector<glm::dmat4>& transforms)
{
    LILA_ZONE();

    if (!mImpl || !mImpl->archive.valid())
        throw std::runtime_error("Alembic writer is not open");
    Impl& im = *mImpl;
    const double spf = 1.0 / im.opts.fps;
    for (size_t i = 0; i < names.size() && i < transforms.size(); ++i)
    {
        const std::string name = objectName(names[i]);
        auto it = im.objects.find(name);
        if (it == im.objects.end())
        {
            const geo::Mesh* shape = i < shapes.size() ? shapes[i] : nullptr;
            if (!shape || shape->numFaces() == 0)
                continue;
            uint32_t ts = im.archive.addTimeSampling(AbcG::TimeSampling(spf, im.opts.startTime + im.frames * spf));
            Object o;
            o.xform = AbcG::OXform(im.archive.getTop(), name, ts);
            o.mesh = AbcG::OPolyMesh(o.xform, name + "Shape", ts);
            it = im.objects.emplace(name, std::move(o)).first;
            im.order.push_back(name);
            im.write(it->second, shape);
        }
        const glm::dmat4& m = transforms[i];
        Imath::M44d mat;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                mat[c][r] = m[c][r];
        AbcG::XformSample sample;
        sample.setMatrix(mat);
        it->second.xform.getSchema().set(sample);
    }
    ++im.frames;
}

void Writer::close()
{
    if (!mImpl)
        return;
    mImpl->objects.clear();
    mImpl->archive = AbcG::OArchive();
}

std::vector<ObjectInfo> read(const fs::path& path, std::string* error)
{
    LILA_ZONE();

    std::vector<ObjectInfo> out;
    try
    {
        Alembic::AbcCoreFactory::IFactory factory;
        AbcG::IArchive archive = factory.getArchive(path.string());
        if (!archive.valid())
        {
            if (error)
                *error = "not an Alembic file: " + path.string();
            return out;
        }
        std::function<void(const AbcG::IObject&)> walk = [&](const AbcG::IObject& obj) {
            for (size_t i = 0; i < obj.getNumChildren(); ++i)
            {
                AbcG::IObject child = obj.getChild(i);
                if (AbcG::IPolyMesh::matches(child.getHeader()))
                {
                    AbcG::IPolyMesh mesh(child, Alembic::Abc::kWrapExisting);
                    AbcG::IPolyMeshSchema& s = mesh.getSchema();
                    ObjectInfo info;
                    info.path = child.getFullName();
                    info.samples = s.getNumSamples();
                    info.normals = s.getNormalsParam().valid();
                    info.uvs = s.getUVsParam().valid();
                    AbcG::ICompoundProperty arb = s.getArbGeomParams();
                    info.colors = arb.valid() && arb.getPropertyHeader("Cd") != nullptr;
                    AbcG::TimeSamplingPtr ts = s.getTimeSampling();
                    for (size_t k = 0; k < info.samples; ++k)
                    {
                        AbcG::IPolyMeshSchema::Sample sample;
                        s.get(sample, Alembic::Abc::ISampleSelector(Alembic::Abc::index_t(k)));
                        info.points.push_back(sample.getPositions() ? sample.getPositions()->size() : 0);
                        info.faces.push_back(sample.getFaceCounts() ? sample.getFaceCounts()->size() : 0);
                        info.times.push_back(ts->getSampleTime(Alembic::Abc::index_t(k)));
                        if (k == 0 && sample.getPositions())
                            for (size_t p = 0; p < sample.getPositions()->size(); ++p)
                            {
                                const Imath::V3f& v = (*sample.getPositions())[p];
                                info.firstPositions.emplace_back(v.x, v.y, v.z);
                            }
                    }
                    out.push_back(std::move(info));
                }
                walk(child);
            }
        };
        walk(archive.getTop());
    }
    catch (const std::exception& e)
    {
        if (error)
            *error = e.what();
        out.clear();
    }
    return out;
}
} // namespace lila::seq::abc

namespace lila::seq::abc
{
namespace
{
glm::dmat4 toGlm(const Imath::M44d& m)
{
    glm::dmat4 out(1.0);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i][j] = m[i][j];
    return out;
}

/** Facevarying values (in the file's corner order) per written corner, or per point; empty when neither fits. */
template <class G, class V, class F>
std::vector<G> cornerValues(const V& vals, AbcG::GeometryScope scope, size_t corners, const std::vector<int32_t>& indices, F conv)
{
    std::vector<G> out;
    if (!vals)
        return out;
    if ((scope == AbcG::kFacevaryingScope || scope == AbcG::kUnknownScope) && vals->size() == corners)
        for (size_t c = 0; c < corners; ++c)
            out.push_back(conv((*vals)[c]));
    else if (scope == AbcG::kVertexScope || scope == AbcG::kVaryingScope)
        for (int32_t p : indices)
            if (p >= 0 && size_t(p) < vals->size())
                out.push_back(conv((*vals)[size_t(p)]));
    if (out.size() != corners)
        out.clear();
    return out;
}
} // namespace

std::vector<MeshSample> readMeshes(const fs::path& path, double time, const std::string& filter, std::string* error)
{
    LILA_ZONE();

    std::vector<MeshSample> out;
    try
    {
        Alembic::AbcCoreFactory::IFactory factory;
        AbcG::IArchive archive = factory.getArchive(path.string());
        if (!archive.valid())
        {
            if (error)
                *error = "not an Alembic file: " + path.string();
            return out;
        }
        const Alembic::Abc::ISampleSelector sel(time, Alembic::Abc::ISampleSelector::kNearIndex);
        std::function<void(const AbcG::IObject&, const glm::dmat4&)> walk = [&](const AbcG::IObject& obj, const glm::dmat4& parent) {
            for (size_t i = 0; i < obj.getNumChildren(); ++i)
            {
                AbcG::IObject child = obj.getChild(i);
                glm::dmat4 world = parent;
                if (AbcG::IXform::matches(child.getHeader()))
                {
                    AbcG::IXform x(child, Alembic::Abc::kWrapExisting);
                    AbcG::XformSample xs;
                    x.getSchema().get(xs, sel);
                    world = (xs.getInheritsXforms() ? parent : glm::dmat4(1.0)) * toGlm(xs.getMatrix());
                }
                else if (AbcG::IPolyMesh::matches(child.getHeader()) && (filter.empty() || child.getFullName().find(filter) != std::string::npos))
                {
                    AbcG::IPolyMesh pm(child, Alembic::Abc::kWrapExisting);
                    AbcG::IPolyMeshSchema& s = pm.getSchema();
                    AbcG::IPolyMeshSchema::Sample sample;
                    s.get(sample, sel);
                    if (sample.getPositions() && sample.getFaceCounts() && sample.getFaceIndices())
                    {
                        std::vector<glm::dvec3> points;
                        const auto& positions = *sample.getPositions();
                        for (size_t p = 0; p < positions.size(); ++p)
                            points.push_back(glm::dvec3(world * glm::dvec4(positions[p].x, positions[p].y, positions[p].z, 1.0)));
                        const auto& counts = *sample.getFaceCounts();
                        const auto& idx = *sample.getFaceIndices();
                        std::vector<int32_t> written(idx.get(), idx.get() + idx.size());
                        // Faces back to counter-clockwise: each face's corners reversed, remembering where each came from.
                        std::vector<Index> sizes, ccw;
                        std::vector<size_t> source;
                        size_t at = 0;
                        for (size_t f = 0; f < counts.size(); ++f)
                        {
                            const size_t n = size_t(counts[f]);
                            sizes.push_back(Index(n));
                            for (size_t k = 0; k < n; ++k)
                            {
                                ccw.push_back(Index(written[at + n - 1 - k]));
                                source.push_back(at + n - 1 - k);
                            }
                            at += n;
                        }
                        MeshSample ms;
                        ms.path = child.getFullName();
                        ms.mesh = geo::Mesh::fromPolygons(points, sizes, ccw);
                        std::vector<glm::vec2> uvs;
                        std::vector<glm::vec3> normals;
                        if (AbcG::IV2fGeomParam uvp = s.getUVsParam(); uvp.valid())
                        {
                            AbcG::IV2fGeomParam::Sample us = uvp.getExpandedValue(sel);
                            uvs = cornerValues<glm::vec2>(us.getVals(), uvp.getScope(), idx.size(), written, [](const Imath::V2f& v) { return glm::vec2(v.x, v.y); });
                        }
                        if (AbcG::IN3fGeomParam np = s.getNormalsParam(); np.valid())
                        {
                            AbcG::IN3fGeomParam::Sample ns = np.getExpandedValue(sel);
                            normals = cornerValues<glm::vec3>(ns.getVals(), np.getScope(), idx.size(), written, [](const Imath::V3f& v) { return glm::vec3(v.x, v.y, v.z); });
                        }
                        // Corners keep the order they were given in when no face was skipped.
                        const geo::Mesh& m = ms.mesh;
                        if (m.numFaces() == counts.size() && m.cornerCapacity() == ccw.size() && (!uvs.empty() || !normals.empty()))
                        {
                            std::span<glm::vec2> uv = uvs.empty() ? std::span<glm::vec2>{} : ms.mesh.cornerAttrs().getOrAdd<glm::vec2>("uv0");
                            std::span<glm::vec3> nn = normals.empty() ? std::span<glm::vec3>{} : ms.mesh.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal);
                            const glm::dmat3 nm = glm::transpose(glm::inverse(glm::dmat3(world)));
                            for (size_t c = 0; c < ccw.size(); ++c)
                            {
                                if (!uv.empty())
                                    uv[c] = uvs[source[c]];
                                if (!nn.empty())
                                    nn[c] = glm::vec3(glm::normalize(nm * glm::dvec3(normals[source[c]])));
                            }
                        }
                        out.push_back(std::move(ms));
                    }
                }
                walk(child, world);
            }
        };
        walk(archive.getTop(), glm::dmat4(1.0));
    }
    catch (const std::exception& e)
    {
        if (error)
            *error = e.what();
        out.clear();
    }
    return out;
}
} // namespace lila::seq::abc
