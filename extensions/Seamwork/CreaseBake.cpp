#include "Seams.h"

#include <lila/Bake.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>

namespace seamwork
{
namespace
{
namespace bake = lila::bake;

constexpr size_t kBand = 1 << 16;

/** The ground under the objects' bounds plus reach, as a mesh in its own space; empty when the objects miss it. */
lila::geo::Mesh groundPatch(const Ground& g, std::span<const Object> objects, double reach)
{
    const lila::geo::Heightfield& h = *g.field;
    const glm::dmat4 inv = glm::inverse(g.world);
    glm::dvec2 lo(1e300), hi(-1e300);
    for (const Object& ob : objects)
    {
        if (ob.ignore || ob.ground || !ob.mesh)
            continue;
        for (lila::geo::Index p = 0; p < ob.mesh->pointCapacity(); ++p)
            if (ob.mesh->pointValid(p))
            {
                const glm::dvec3 l = glm::dvec3(inv * ob.world * glm::dvec4(ob.mesh->position(p), 1.0));
                lo = glm::min(lo, glm::dvec2(l.x, l.z));
                hi = glm::max(hi, glm::dvec2(l.x, l.z));
            }
    }
    if (lo.x > hi.x)
        return {};
    const int x0 = std::max(0, int(std::floor((lo.x - reach - h.origin.x) / h.cell)));
    const int z0 = std::max(0, int(std::floor((lo.y - reach - h.origin.y) / h.cell)));
    const int x1 = std::min(h.nx - 1, int(std::ceil((hi.x + reach - h.origin.x) / h.cell)));
    const int z1 = std::min(h.nz - 1, int(std::ceil((hi.y + reach - h.origin.y) / h.cell)));
    if (x1 - x0 < 1 || z1 - z0 < 1)
        return {};
    lila::geo::Heightfield crop = lila::geo::makeHeightfield(x1 - x0 + 1, z1 - z0 + 1, h.cell, h.samplePos(x0, z0));
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            crop.heights[crop.index(x - x0, z - z0)] = h.at(x, z);
    lila::geo::HeightfieldMeshOptions mo;
    mo.uvs = false;
    mo.masks = false;
    mo.holes.clear();
    return lila::geo::heightfieldToMesh(crop, mo);
}

void addHigh(bake::GroupInputs& g, const lila::geo::Mesh& mesh, const glm::dmat4& world, std::string name)
{
    bake::HighInput h;
    h.soup = bake::soupFromMesh(mesh, world, std::move(name));
    if (h.soup.triangleCount() == 0)
        return;
    h.source = h.soup.name;
    h.sourceTriangles = h.soup.triangleCount();
    g.highMin = glm::min(g.highMin, h.soup.boundsMin);
    g.highMax = glm::max(g.highMax, h.soup.boundsMax);
    bake::HighEntry e;
    e.node = h.source;
    g.settings.highs.push_back(e);
    g.highs.push_back(std::move(h));
}
} // namespace

std::string bakeCrease(std::span<const Object> objects, const Ground* ground, const std::vector<std::vector<glm::dvec3>>& points,
                       const std::vector<std::vector<glm::dvec3>>& normals, const Options& o, std::vector<std::vector<float>>& out, std::string& note,
                       const std::string& backendName)
{
    LILA_ZONE();
    const std::vector<std::string> names = bake::backendNames();
    if (std::ranges::find(names, backendName) == names.end())
    {
        note = "no GPU bake backend in this session (batch, or the renderer hasn't started)";
        return {};
    }
    std::unique_ptr<bake::Backend> backend = bake::createBackend(backendName);
    if (!backend)
    {
        note = "the " + backendName + " bake backend could not be created";
        return {};
    }

    bake::BakeInputs inputs;
    bake::GroupInputs& g = inputs.groups.emplace_back();
    g.name = "contacts";
    g.settings.aoScope = "group";
    g.highMin = glm::vec3(1e30f);
    g.highMax = glm::vec3(-1e30f);
    for (size_t i = 0; i < objects.size(); ++i)
        if (!objects[i].ignore && objects[i].mesh)
            addHigh(g, *objects[i].mesh, objects[i].world, objects[i].name.empty() ? "object" + std::to_string(i) : objects[i].name);
    if (ground && ground->field && !ground->field->empty())
        if (const lila::geo::Mesh patch = groundPatch(*ground, objects, o.creaseRange * 2.0); patch.numFaces() > 0)
            addHigh(g, patch, ground->world, ground->name.empty() ? "ground" : ground->name);
    if (g.highs.empty())
    {
        note = "nothing to trace";
        return {};
    }
    inputs.highMin = g.highMin;
    inputs.highMax = g.highMax;
    try
    {
        backend->prepare(inputs);
    }
    catch (const std::exception& e)
    {
        note = "the " + backendName + " bake backend failed to prepare: " + e.what();
        return {};
    }
    if (backendName != "cpu" && backend->mode() == "cpu")
    {
        note = "the GPU bake backend fell back to the CPU";
        return {};
    }

    const bake::MapDef* ao = bake::findMap("ao");
    if (!ao)
    {
        note = "the bake's ao map isn't registered";
        return {};
    }
    bake::MapSettings ms;
    ms.map = "ao";
    ms.options = {{"rays", std::max(4, o.creaseRays)}, {"distance", o.creaseRange}, {"scope", "group"}};
    bake::MapContext ctx{ms, inputs, g};
    ctx.groupIndex = 0;
    ctx.tracer = backend->tracer();
    // The projection ray starts just off the point and runs back through it, so its hit is the point's own surface.
    const float lift = std::max(0.05f, 1e-5f * glm::length(g.highMax - g.highMin));

    out.assign(objects.size(), {});
    std::vector<bake::TexelSample> samples;
    std::vector<bake::HitRecord> hits;
    std::vector<glm::vec4> values;
    std::vector<uint8_t> valid;
    std::vector<std::pair<size_t, size_t>> owner;
    auto flush = [&]() {
        if (samples.empty())
            return;
        hits.assign(samples.size(), {});
        values.assign(samples.size(), glm::vec4(1.0f));
        valid.assign(samples.size(), 0);
        backend->traceProjection(0, samples, hits);
        backend->shade(*ao, ctx, samples, hits, values, valid);
        backend->endBand(samples);
        for (size_t k = 0; k < samples.size(); ++k)
            out[owner[k].first][owner[k].second] = std::clamp(1.0f - values[k].x, 0.0f, 1.0f);
        samples.clear();
        owner.clear();
    };
    for (size_t i = 0; i < points.size(); ++i)
    {
        if (points[i].empty())
            continue;
        out[i].assign(points[i].size(), 0.0f);
        for (size_t p = 0; p < points[i].size(); ++p)
        {
            const glm::dvec3 n = normals[i][p];
            if (n == glm::dvec3(0.0))
                continue;
            bake::TexelSample s{};
            const glm::vec3 pos(points[i][p]), nf(n), origin = pos + nf * lift;
            for (int c = 0; c < 3; ++c)
            {
                s.position[c] = pos[c];
                s.normal[c] = nf[c];
                s.origin[c] = origin[c];
                s.direction[c] = -nf[c];
            }
            s.rayLength = lift * 2.0f;
            s.texel = uint32_t(samples.size());
            s.tile = uint32_t(i);
            samples.push_back(s);
            owner.emplace_back(i, p);
            if (samples.size() == kBand)
                flush();
        }
    }
    flush();
    return backend->mode();
}
} // namespace seamwork
