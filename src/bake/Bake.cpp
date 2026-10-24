#include "bake/Bake.h"

#include "bake/AutoDistance.h"
#include "bake/Backend.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/BakePipeline.h"
#include "bake/BakeWrite.h"
#include "bake/Lint.h"
#include "bake/Maps.h"
#include "bake/MapsInternal.h"
#include "bake/Naming.h"
#include "bake/TexelRaster.h"
#include "core/ImageIO.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "core/Workspace.h"
#include "io/SceneFile.h"
#include "scene/Graph.h"
#include "scene/Materials.h"

#include <tbb/parallel_for.h>
#include <tbb/task_group.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>

namespace lila::bake
{
namespace fs = std::filesystem;

namespace
{
using Clock = std::chrono::steady_clock;

double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

/** Thread time summed over parallel work, for BakeResult::timings. */
struct ThreadTime
{
    std::atomic<int64_t> ns{0};

    void add(Clock::time_point t0) { ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count(); }
    double seconds() const { return double(ns.load()) * 1e-9; }
};

void addTiming(json& timings, const std::string& key, double seconds) { timings[key] = timings.value(key, 0.0) + seconds; }

std::map<scene::NodeId, json>& lastResults()
{
    static std::map<scene::NodeId, json> r;
    return r;
}

std::string shortName(const std::string& path)
{
    const size_t bar = path.rfind('|');
    return bar == std::string::npos ? path : path.substr(bar + 1);
}

struct PlannedMap
{
    MapSettings settings;
    const MapDef* def = nullptr;
    /** Baked only as a pack's source: not written or listed. */
    bool hidden = false;
    /** Packs: the four channels and the planned map each copies (-1: its constant). */
    std::vector<PackChannel> pack;
    std::vector<int> packSource;
};

/** The maps this bake runs: the project's enabled maps, or the requested ones (with defaults when the project lacks them). */
std::vector<PlannedMap> plannedMaps(const ProjectSettings& ps, const BakeOptions& opt, const Backend& backend, std::vector<std::string>& warnings)
{
    std::vector<PlannedMap> out;
    auto add = [&](const MapSettings& m) {
        const MapDef* def = findMap(m.map);
        if (!def)
        {
            warnings.push_back("map '" + m.map + "' is not implemented yet; skipped");
            return;
        }
        if (def->secondaryRays && !backend.tracer())
        {
            warnings.push_back("map '" + m.map + "' needs secondary rays, which the " + backend.name() + " backend lacks; skipped");
            return;
        }
        out.push_back({m, def});
    };
    if (opt.maps.empty())
    {
        for (const MapSettings& m : ps.maps)
            if (m.enabled)
                add(m);
        return out;
    }
    for (const std::string& id : opt.maps)
    {
        auto it = std::ranges::find_if(ps.maps, [&](const MapSettings& m) { return m.map == id; });
        add(it != ps.maps.end() ? *it : MapSettings{.map = id});
    }
    return out;
}

/**
 * Adds what the planned packs read: each channel's map reuses a planned map with the same options, else joins hidden
 * with the project's settings for that map (the channel's options merged over them).
 */
std::vector<PlannedMap> withPacks(std::vector<PlannedMap> maps, const ProjectSettings& ps, const Backend& backend, std::vector<std::string>& warnings)
{
    const size_t planned = maps.size();
    for (size_t p = 0; p < planned; ++p)
    {
        if (maps[p].def->pack.is_null())
            continue;
        maps[p].pack = packChannels(maps[p].settings, *maps[p].def, warnings);
        maps[p].packSource.assign(4, -1);
        for (size_t c = 0; c < 4; ++c)
        {
            const PackChannel& ch = maps[p].pack[c];
            if (ch.map.empty())
                continue;
            const MapDef* def = findMap(ch.map);
            const std::string where = "pack " + maps[p].settings.map + " channel " + std::to_string(c) + ": map '" + ch.map + "' ";
            if (!def || !def->pack.is_null())
            {
                warnings.push_back(where + (def ? "is itself a pack" : "is not implemented") + "; using " + std::to_string(ch.constant));
                continue;
            }
            if (def->secondaryRays && !backend.tracer())
            {
                warnings.push_back(where + "needs secondary rays, which the " + backend.name() + " backend lacks; using " + std::to_string(ch.constant));
                continue;
            }
            auto proj = std::ranges::find_if(ps.maps, [&](const MapSettings& m) { return m.map == ch.map; });
            MapSettings want = proj != ps.maps.end() ? *proj : MapSettings{.map = ch.map};
            if (ch.hasOptions)
                want.options.update(ch.options);
            int found = -1;
            for (size_t i = 0; i < maps.size() && found < 0; ++i)
                if (maps[i].def == def && maps[i].settings.options == want.options)
                    found = int(i);
            if (found < 0)
            {
                PlannedMap h{want, def};
                h.hidden = true;
                maps.push_back(std::move(h));
                found = int(maps.size() - 1);
            }
            maps[p].packSource[c] = found;
        }
    }
    return maps;
}

/** Collects a band's exact-map votes: per texel, the most frequent colour per texture, ties to the lowest sub-sample. */
void voteExact(std::span<const TexelSample> samples, std::span<const uint32_t> order, std::span<const glm::vec4> out,
               std::span<const uint8_t> valid, const std::vector<TextureBuffer*>& texOf)
{
    struct Vote
    {
        TextureBuffer* tex;
        uint32_t colour;
        uint16_t count;
    };
    std::vector<Vote> votes;
    for (size_t a = 0; a < order.size();)
    {
        const uint32_t texel = samples[order[a]].texel;
        size_t b = a;
        votes.clear();
        for (; b < order.size() && samples[order[b]].texel == texel; ++b)
        {
            const uint32_t i = order[b];
            TextureBuffer* tex = texOf[i];
            if (!tex)
                continue;
            ++tex->samples[texel];
            if (!valid[i])
                continue;
            const uint32_t c = packRgba8(out[i]);
            auto it = std::ranges::find_if(votes, [&](const Vote& v) { return v.tex == tex && v.colour == c; });
            if (it == votes.end())
                votes.push_back({tex, c, 1});
            else
                ++it->count;
        }
        // Votes are in sub-sample order, so a strictly larger count is needed to beat an earlier colour.
        for (size_t k = 0; k < votes.size(); ++k)
        {
            const Vote& v = votes[k];
            bool best = true;
            for (size_t m = 0; m < votes.size() && best; ++m)
                if (m != k && votes[m].tex == v.tex && (votes[m].count > v.count || (votes[m].count == v.count && m < k)))
                    best = false;
            if (best && v.count > v.tex->votes[texel])
            {
                v.tex->votes[texel] = v.count;
                v.tex->exact[texel] = v.colour;
            }
        }
        a = b;
    }
}
} // namespace

json OutputFile::toJson() const
{
    return {{"file", paths::toUtf8(file)}, {"map", map},   {"suffix", suffix},       {"udim", udim},           {"width", width},
            {"height", height},            {"bits", bits}, {"format", format},       {"groups", groups},       {"meshes", meshes},
            {"materials", materials},      {"meta", meta},  {"layer", layer},
            {"colorSpace", colorSpace},    {"normalFormat", normalFormat}};
}

json GroupStats::toJson() const
{
    return {{"group", group},         {"samples", samples},         {"hits", hits},
            {"misses", misses},       {"backfaces", backfaces},     {"highTriangles", highTriangles},
            {"lowTriangles", lowTriangles}, {"frontal", frontal},       {"rear", rear},
            {"frontalAuto", frontalAuto}, {"rearAuto", rearAuto},       {"measured", measured}};
}

json BakeResult::toJson() const
{
    json outs = json::array();
    for (const OutputFile& o : outputs)
        outs.push_back(o.toJson());
    json gs = json::array();
    uint64_t samples = 0, hits = 0, misses = 0, backfaces = 0;
    for (const GroupStats& g : groups)
    {
        gs.push_back(g.toJson());
        samples += g.samples;
        hits += g.hits;
        misses += g.misses;
        backfaces += g.backfaces;
    }
    return {{"outputs", outs},
            {"stats", {{"samples", samples}, {"hits", hits}, {"misses", misses}, {"backfaces", backfaces}, {"groups", gs}}},
            {"warnings", warnings},
            {"manifest", manifest.empty() ? json() : json(paths::toUtf8(manifest))},
            {"dryRun", dryRun},
            {"seconds", seconds},
            {"inputHash", inputHash},
            {"lint", lint},
            {"timings", timings},
            {"backend", backend},
            {"backendMode", backendMode}};
}

fs::path resolveFolder(const std::string& folder)
{
    fs::path p = paths::fromUtf8(folder.empty() ? std::string("bake") : folder);
    if (p.is_relative())
    {
        fs::path base = fs::current_path();
        if (Workspace::get().isOpen())
            base = Workspace::get().root();
        else if (!io::currentPath().empty())
            base = io::currentPath().parent_path();
        p = base / p;
    }
    return p.lexically_normal();
}

BakeResult runBake(const BakeInputs& in, const ProjectSettings& ps, const BakeOptions& opt, Backend& backend)
{
    LILA_ZONE();

    const auto started = std::chrono::steady_clock::now();
    registerBuiltinMaps();
    BakeResult result;
    result.dryRun = opt.dryRun;
    result.warnings = in.warnings;
    const OutputSettings& os = ps.output;
    for (const std::string& t : unknownTokens(os.name))
        result.warnings.push_back("unknown token {" + t + "} in the output name is kept as written");
    const std::vector<PlannedMap> maps = withPacks(plannedMaps(ps, opt, backend, result.warnings), ps, backend, result.warnings);

    // Maps the backend keeps on the device from raster to resolved texture; the rest go band by band through shade().
    std::vector<uint8_t> onDevice(maps.size(), 0);
    std::vector<size_t> residentOf(maps.size(), SIZE_MAX);
    std::unique_ptr<ResidentBake> resident;
    if (!opt.dryRun)
    {
        std::vector<ResidentMap> residentMaps;
        for (size_t m = 0; m < maps.size(); ++m)
            if (maps[m].def->pack.is_null() && backend.residentSupports(*maps[m].def, maps[m].settings))
            {
                residentOf[m] = residentMaps.size();
                residentMaps.push_back({maps[m].def, maps[m].settings});
            }
        if (!residentMaps.empty())
            resident = backend.beginResident(in, residentMaps, os);
        for (size_t m = 0; m < maps.size() && resident; ++m)
            onDevice[m] = residentOf[m] != SIZE_MAX;
    }
    bool bandMaps = false;
    for (size_t m = 0; m < maps.size(); ++m)
        bandMaps = bandMaps || (maps[m].def->pack.is_null() && !onDevice[m]);

    RasterSettings rs;
    rs.width = os.width;
    rs.height = os.height;
    rs.supersampling = os.supersampling;
    rs.bandRows = std::max(1, resident && !bandMaps ? resident->bandRows(os.width, os.supersampling) : backend.bandRows(os.width, os.supersampling));

    // Rasters first: they decide which tiles and materials each low writes, which decides the files.
    struct LowJob
    {
        size_t group;
        size_t low;
        std::unique_ptr<TexelRaster> raster;
        bool forceUdim = false;
    };
    std::vector<LowJob> lowJobs;
    for (size_t g = 0; g < in.groups.size(); ++g)
        for (size_t l = 0; l < in.groups[g].lows.size(); ++l)
        {
            const LowInput& low = in.groups[g].lows[l];
            if (low.soup.uvs.empty())
            {
                result.warnings.push_back(low.soup.name + " has no uv0; skipped");
                continue;
            }
            LowJob j{g, l, std::make_unique<TexelRaster>(low, rs)};
            if (j.raster->skippedTriangles())
                result.warnings.push_back(low.soup.name + ": " + std::to_string(j.raster->skippedTriangles()) +
                                          " triangle(s) outside UDIM tiles 1001-1999 or without UV area were skipped");
            j.forceUdim = j.raster->udims().size() > 1 && !hasToken(os.name, "udim");
            lowJobs.push_back(std::move(j));
        }

    std::map<std::string, std::unique_ptr<TextureBuffer>> textures;
    std::vector<std::string> textureOrder;
    // PSD textures: the layered document each one becomes a layer of.
    std::map<std::string, fs::path> layered;
    std::set<std::string> hiddenKeys;
    // Per pack texture, the texture each channel copies (null: a constant).
    std::map<TextureBuffer*, std::vector<const TextureBuffer*>> packSources;
    auto textureFor = [&](const LowJob& j, int udim, const PlannedMap& m, const std::string& material) -> TextureBuffer* {
        const GroupInputs& gi = in.groups[j.group];
        const LowInput& low = gi.lows[j.low];
        NameTokens t;
        t.project = in.project;
        t.group = gi.name;
        t.mesh = shortName(low.soup.name);
        // A mesh without materials has only the fallback slot; its texture set is better named after the mesh.
        t.material = material == scene::materials::kDefaultSlot && low.soup.materialNames.size() == 1 ? t.mesh : material;
        t.map = m.settings.suffix.empty() ? m.def->suffix : m.settings.suffix;
        const std::string format = m.settings.format.empty() ? os.format : m.settings.format;
        t.ext = formatExt(format);
        t.udim = udim;
        t.udimStyle = os.udimNaming;
        t.width = os.width;
        t.height = os.height;
        const fs::path file = opt.folder / paths::fromUtf8(expandName(os.name, t, j.forceUdim));
        // Hidden pack sources never reach disk, so they must not merge with a real texture of the same name.
        const std::string key = (m.hidden ? "\x01" + std::to_string(&m - maps.data()) + ":" : std::string()) + paths::toUtf8(file);
        auto it = textures.find(key);
        if (it == textures.end())
        {
            auto tex = std::make_unique<TextureBuffer>();
            tex->file = file;
            tex->def = m.def;
            tex->settings = m.settings;
            tex->width = os.width;
            tex->height = os.height;
            tex->udim = udim;
            tex->format = t.ext;
            const int bits = m.settings.bits ? m.settings.bits : os.bits;
            tex->bits = formatBits(t.ext, bits);
            if (m.hidden)
                hiddenKeys.insert(key);
            if (t.ext == "psd")
                layered[key] = opt.folder / paths::fromUtf8(layeredName(os.name, t, j.forceUdim));
            tex->samplesPerTexel = j.raster->samplesPerTexel();
            if (!opt.dryRun && !onDevice[size_t(&m - maps.data())])
                tex->allocate();
            it = textures.emplace(key, std::move(tex)).first;
            textureOrder.push_back(key);
        }
        else if (it->second->def != m.def)
            result.warnings.push_back("maps " + it->second->def->id + " and " + m.def->id + " both write " + key + "; add {map} to the name");
        TextureBuffer* tex = it->second.get();
        tex->groups.insert(gi.name);
        tex->meshes.insert(low.soup.name);
        tex->materials.insert(material);
        return tex;
    };

    // texOf[job][udim index][map][material] for the band loop.
    std::vector<std::vector<std::vector<std::vector<TextureBuffer*>>>> lookup(lowJobs.size());
    double totalBands = 0.0;
    for (size_t k = 0; k < lowJobs.size(); ++k)
    {
        const LowJob& j = lowJobs[k];
        const LowInput& low = in.groups[j.group].lows[j.low];
        const std::vector<int>& udims = j.raster->udims();
        lookup[k].resize(udims.size());
        for (size_t u = 0; u < udims.size(); ++u)
        {
            std::vector<uint8_t> used(low.soup.materialNames.size(), 0);
            for (uint32_t t = 0; t < low.soup.triangleCount(); ++t)
                if (j.raster->triangleUdims()[t] == udims[u] && low.soup.materials[t] < used.size())
                    used[low.soup.materials[t]] = 1;
            lookup[k][u].resize(maps.size());
            for (size_t m = 0; m < maps.size(); ++m)
            {
                lookup[k][u][m].assign(low.soup.materialNames.size(), nullptr);
                for (size_t mat = 0; mat < used.size(); ++mat)
                    if (used[mat])
                        lookup[k][u][m][mat] = textureFor(j, udims[u], maps[m], low.soup.materialNames[mat]);
            }
            for (size_t m = 0; m < maps.size(); ++m)
                for (size_t mat = 0; mat < used.size() && !maps[m].packSource.empty(); ++mat)
                    if (TextureBuffer* pack = lookup[k][u][m][mat]; pack && !packSources.contains(pack))
                    {
                        std::vector<const TextureBuffer*>& src = packSources[pack];
                        for (int c : maps[m].packSource)
                            src.push_back(c >= 0 ? lookup[k][u][size_t(c)][mat] : nullptr);
                    }
            totalBands += double(j.raster->bandCount()) * double(int(bandMaps) + int(resident != nullptr));
        }
    }

    // Too many textures to keep on the device at once: the whole bake goes band by band instead.
    if (resident)
    {
        size_t texels = 0;
        for (const auto& [key, tex] : textures)
            if (tex->def->pack.is_null() && tex->sum.empty() && tex->exact.empty())
                texels += tex->texelCount();
        if (!resident->fits(texels))
        {
            result.warnings.push_back("the textures don't fit in GPU memory at once; baking band by band");
            resident.reset();
            std::ranges::fill(onDevice, uint8_t(0));
            bandMaps = std::ranges::any_of(maps, [](const PlannedMap& m) { return m.def->pack.is_null(); });
            rs.bandRows = std::max(1, backend.bandRows(os.width, os.supersampling));
            totalBands = 0.0;
            for (LowJob& j : lowJobs)
            {
                j.raster = std::make_unique<TexelRaster>(in.groups[j.group].lows[j.low], rs);
                totalBands += double(j.raster->bandCount()) * double(j.raster->udims().size());
            }
            for (const auto& [key, tex] : textures)
                if (tex->def->pack.is_null() && tex->sum.empty() && tex->exact.empty())
                    tex->allocate();
        }
    }

    // A texture as the bake result lists it.
    auto outputOf = [&](const std::string& key) {
        const TextureBuffer& tex = *textures.at(key);
        OutputFile o;
        o.file = layered.contains(key) ? layered.at(key) : tex.file;
        o.map = tex.def->id;
        o.suffix = tex.settings.suffix.empty() ? tex.def->suffix : tex.settings.suffix;
        o.udim = tex.udim;
        o.width = tex.width;
        o.height = tex.height;
        o.bits = tex.bits;
        o.format = tex.format;
        if (layered.contains(key))
            o.layer = o.suffix;
        const json info = textureInfo(tex);
        o.colorSpace = info.value("colorSpace", std::string("linear"));
        o.normalFormat = info.value("normalFormat", std::string());
        o.groups.assign(tex.groups.begin(), tex.groups.end());
        o.meshes.assign(tex.meshes.begin(), tex.meshes.end());
        o.materials.assign(tex.materials.begin(), tex.materials.end());
        o.meta = tex.meta;
        return o;
    };

    std::vector<GroupStats> stats(in.groups.size());
    for (size_t g = 0; g < in.groups.size(); ++g)
    {
        stats[g].group = in.groups[g].name;
        stats[g].frontal = in.groups[g].frontal;
        stats[g].rear = in.groups[g].rear;
        stats[g].frontalAuto = in.groups[g].settings.frontal <= 0.0;
        stats[g].rearAuto = in.groups[g].settings.rear <= 0.0;
        stats[g].measured = in.groups[g].measured;
        for (const HighInput& h : in.groups[g].highs)
            stats[g].highTriangles += h.soup.triangleCount();
        for (const LowInput& l : in.groups[g].lows)
            stats[g].lowTriangles += l.soup.triangleCount();
    }

    json& timings = result.timings;
    addTiming(timings, "plan", since(started));
    if (!opt.dryRun && !maps.empty())
    {
        progress::Scope scope("Baking " + in.project, totalBands + double(textures.size()), progress::Cancellable);
        std::vector<std::shared_ptr<const void>> prepared(maps.size());
        for (size_t m = 0; m < maps.size(); ++m)
            if (maps[m].def->prepare && !onDevice[m])
            {
                const auto t0 = Clock::now();
                prepared[m] = maps[m].def->prepare(maps[m].settings, in);
                addTiming(timings, "prepare:" + maps[m].def->id, since(t0));
            }
        ThreadTime tRaster, tTrace, tAccum;
        std::vector<ThreadTime> tMap(maps.size());
        const auto bandsStarted = Clock::now();
        if (resident)
        {
            scope.setStatus("Baking on the GPU");
            for (size_t k = 0; k < lowJobs.size(); ++k)
            {
                const LowJob& j = lowJobs[k];
                const GroupInputs& gi = in.groups[j.group];
                for (size_t u = 0; u < j.raster->udims().size(); ++u)
                {
                    ResidentTile tile;
                    tile.group = j.group;
                    tile.low = j.low;
                    tile.udim = j.raster->udims()[u];
                    tile.raster = j.raster.get();
                    tile.cage = {gi.frontal, gi.rear, float(gi.settings.cageOffset), float(gi.settings.skew)};
                    for (size_t m = 0; m < maps.size(); ++m)
                        if (onDevice[m])
                        {
                            tile.targets.resize(std::max(tile.targets.size(), residentOf[m] + 1));
                            tile.targets[residentOf[m]] = lookup[k][u][m];
                        }
                    tile.onBand = [&](int rows) {
                        scope.checkCancel();
                        scope.step(double(rows) / double(rs.bandRows));
                    };
                    tile.cancelled = [&scope] { return scope.cancelled(); };
                    const ResidentStats r = resident->bakeTile(tile);
                    stats[j.group].samples += r.samples;
                    stats[j.group].hits += r.hits;
                    stats[j.group].misses += r.samples - r.hits;
                    stats[j.group].backfaces += r.backfaces;
                }
            }
            addTiming(timings, "gpu", since(bandsStarted));
        }
        for (size_t k = 0; k < lowJobs.size() && bandMaps; ++k)
        {
            const LowJob& j = lowJobs[k];
            const GroupInputs& gi = in.groups[j.group];
            const LowInput& low = gi.lows[j.low];
            const CageParams cage{gi.frontal, gi.rear, float(gi.settings.cageOffset), float(gi.settings.skew)};
            std::atomic<uint64_t> nSamples{0}, nHits{0}, nMisses{0}, nBack{0};
            const std::shared_ptr<progress::Node> parent = progress::current();
            for (size_t u = 0; u < j.raster->udims().size(); ++u)
            {
                const int udim = j.raster->udims()[u];
                const auto& texOf = lookup[k][u];
                tbb::parallel_for(size_t(0), j.raster->bandCount(), [&](size_t band) {
                    progress::Adopt adopt(parent);
                    scope.checkCancel();
                    std::vector<TexelSample> samples;
                    auto t0 = Clock::now();
                    j.raster->band(udim, band, cage, samples);
                    tRaster.add(t0);
                    if (samples.empty())
                    {
                        scope.step();
                        return;
                    }
                    for (TexelSample& s : samples)
                        s.low = uint32_t(j.low);
                    std::vector<HitRecord> hits(samples.size());
                    t0 = Clock::now();
                    backend.traceProjection(j.group, samples, hits);
                    tTrace.add(t0);
                    uint64_t h = 0, b = 0;
                    for (const HitRecord& r : hits)
                    {
                        h += (r.flags & kHitValid) ? 1 : 0;
                        b += (r.flags & kHitBackface) ? 1 : 0;
                    }
                    nSamples += samples.size();
                    nHits += h;
                    nMisses += samples.size() - h;
                    nBack += b;

                    std::vector<uint32_t> order;
                    std::vector<glm::vec4> out(samples.size());
                    std::vector<uint8_t> valid(samples.size());
                    std::vector<TextureBuffer*> sampleTex(samples.size());
                    for (size_t m = 0; m < maps.size(); ++m)
                    {
                        if (!maps[m].def->pack.is_null() || onDevice[m])
                            continue;
                        const MapContext ctx{maps[m].settings, in, gi, j.group, backend.tracer(), os.width, os.height, prepared[m].get()};
                        std::fill(valid.begin(), valid.end(), uint8_t(0));
                        t0 = Clock::now();
                        backend.shade(*maps[m].def, ctx, samples, hits, out, valid);
                        tMap[m].add(t0);
                        t0 = Clock::now();
                        for (size_t i = 0; i < samples.size(); ++i)
                        {
                            const uint32_t mat = low.soup.materials[samples[i].lowTriangle];
                            sampleTex[i] = mat < texOf[m].size() ? texOf[m][mat] : nullptr;
                        }
                        if (maps[m].def->resolve == Resolve::Exact)
                        {
                            if (order.empty())
                            {
                                order.resize(samples.size());
                                std::iota(order.begin(), order.end(), 0u);
                                std::ranges::stable_sort(order, [&](uint32_t a, uint32_t c) {
                                    return samples[a].texel != samples[c].texel ? samples[a].texel < samples[c].texel : samples[a].sub < samples[c].sub;
                                });
                            }
                            voteExact(samples, order, out, valid, sampleTex);
                            tAccum.add(t0);
                            continue;
                        }
                        for (size_t i = 0; i < samples.size(); ++i)
                        {
                            TextureBuffer* tex = sampleTex[i];
                            if (!tex)
                                continue;
                            const uint32_t texel = samples[i].texel;
                            ++tex->samples[texel];
                            if (valid[i])
                            {
                                tex->sum[texel] += out[i];
                                tex->weight[texel] += 1.0f;
                            }
                        }
                        tAccum.add(t0);
                    }
                    backend.endBand(samples);
                    scope.step();
                });
            }
            if (!resident)
            {
                stats[j.group].samples += nSamples;
                stats[j.group].hits += nHits;
                stats[j.group].misses += nMisses;
                stats[j.group].backfaces += nBack;
            }
        }

        if (bandMaps)
        {
            addTiming(timings, "bands", since(bandsStarted));
            addTiming(timings, "raster", tRaster.seconds());
            addTiming(timings, "trace", tTrace.seconds());
            addTiming(timings, "accumulate", tAccum.seconds());
        }
        for (size_t m = 0; m < maps.size() && bandMaps; ++m)
            if (!onDevice[m] && maps[m].def->pack.is_null())
                addTiming(timings, "map:" + maps[m].def->id, tMap[m].seconds());
        scope.setStatus("Writing textures");
        if (opt.prepareWrite && !opt.keepTexture)
        {
            std::vector<fs::path> files;
            for (const std::string& key : textureOrder)
                if (!hiddenKeys.contains(key))
                    files.push_back(layered.contains(key) ? layered[key] : textures[key]->file);
            opt.prepareWrite(files);
        }
        TextureWriter writer(os, opt.inputHash);
        // Packs go last: they copy other textures once those are finalized and dilated.
        std::vector<std::string> writeOrder;
        for (int pass = 0; pass < 2; ++pass)
            for (const std::string& key : textureOrder)
                if (textures[key]->def->pack.is_null() == (pass == 0))
                    writeOrder.push_back(key);
        // Textures finish one at a time (the GPU's work is serial anyway) while finished ones encode and write on workers.
        tbb::task_group writes;
        std::mutex writeMutex;
        std::exception_ptr writeError;
        ThreadTime tWrite;
        auto finishAll = [&] {
            for (const std::string& key : writeOrder)
            {
                scope.checkCancel();
                TextureBuffer& tex = *textures[key];
                if (!tex.def->pack.is_null())
                {
                    auto pm = std::ranges::find_if(maps, [&](const PlannedMap& m) { return m.def == tex.def && m.settings.options == tex.settings.options; });
                    composePack(tex, pm != maps.end() ? pm->pack : std::vector<PackChannel>(4), packSources[&tex], tex.meta);
                }
                else
                {
                    auto pm = std::ranges::find_if(maps, [&](const PlannedMap& m) { return m.def == tex.def && m.settings.options == tex.settings.options; });
                    const bool device = resident && pm != maps.end() && onDevice[size_t(pm - maps.begin())];
                    auto t0 = Clock::now();
                    const bool done = device ? resident->finish(tex, os.padding) : false;
                    if (!device)
                        tex.resolve();
                    addTiming(timings, device ? "finish" : "resolve", since(t0));
                    if (!done)
                    {
                        t0 = Clock::now();
                        if (tex.def->finalize)
                            tex.def->finalize(tex.settings, in, tex, tex.meta);
                        addTiming(timings, "finalize", since(t0));
                        t0 = Clock::now();
                        if (tex.def->dilate)
                            backend.dilate(tex, os.padding);
                        addTiming(timings, "dilate", since(t0));
                    }
                }
                if (hiddenKeys.contains(key) || opt.keepTexture)
                {
                    if (opt.keepTexture && !hiddenKeys.contains(key))
                        opt.keepTexture(tex);
                    scope.step();
                    continue;
                }
                const fs::path into = layered.contains(key) ? layered[key] : fs::path();
                // Layers are only kept until writer.finish(), and keep the order they are added in.
                if (!into.empty())
                {
                    image::Image img{tex.width, tex.height, std::vector<float>(tex.texelCount() * 4)};
                    std::memcpy(img.rgba.data(), tex.value.data(), tex.texelCount() * sizeof(glm::vec4));
                    writer.add(tex, std::move(img), into);
                    scope.step();
                    continue;
                }
                writes.run([&, texPtr = &tex, into, key] {
                    const auto tw = Clock::now();
                    try
                    {
                        const TextureBuffer& t = *texPtr;
                        std::error_code ec;
                        fs::create_directories(t.file.parent_path(), ec);
                        image::Image img{t.width, t.height, std::vector<float>(t.texelCount() * 4)};
                        std::memcpy(img.rgba.data(), t.value.data(), t.texelCount() * sizeof(glm::vec4));
                        writer.add(t, std::move(img), into);
                        if (opt.onWritten)
                            opt.onWritten(outputOf(key));
                    }
                    catch (...)
                    {
                        std::scoped_lock lock(writeMutex);
                        if (!writeError)
                            writeError = std::current_exception();
                    }
                    tWrite.add(tw);
                    scope.step();
                });
            }
        };
        try
        {
            finishAll();
        }
        catch (...)
        {
            writes.wait();
            throw;
        }
        const auto tf = Clock::now();
        writes.wait();
        addTiming(timings, "writeWait", since(tf));
        if (writeError)
            std::rethrow_exception(writeError);
        const auto tl = Clock::now();
        writer.finish();
        addTiming(timings, "write", tWrite.seconds() + since(tl));
        if (resident)
        {
            const json gpu = resident->timings();
            for (const auto& [k, v] : gpu.items())
                addTiming(timings, k, v.get<double>());
        }
    }
    else if (!opt.dryRun)
        result.warnings.push_back("nothing to bake: no maps enabled");

    for (const std::string& key : textureOrder)
    {
        if (hiddenKeys.contains(key))
            continue;
        result.outputs.push_back(outputOf(key));
    }
    result.groups = std::move(stats);
    result.inputHash = opt.inputHash;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    if (!opt.dryRun && !opt.keepTexture && !result.outputs.empty())
    {
        result.manifest = opt.folder / paths::fromUtf8(fileSafe(in.project) + "_bake.json");
        json m = result.toJson();
        m["project"] = in.project;
        m["target"] = ps.target;
        m["settings"] = ps.toJson();
        m["inputHash"] = opt.inputHash;
        std::ofstream f(result.manifest);
        f << m.dump(2);
    }
    return result;
}

/** A bake split at the thread boundary: everything that reads the scene happens before, on the main thread. */
struct PreparedBake
{
    scene::NodeId project = scene::kNoNode;
    std::string name;
    ProjectSettings ps;
    std::unique_ptr<Backend> backend;
    BakeInputs inputs;
    BakeOptions opt;
    /** The lint checks that read the scene (triangulation, cage topology). */
    LintReport sceneLint;
    bool dryRun = false;
    json timings = json::object();
    Clock::time_point started;
};

namespace
{
std::unique_ptr<PreparedBake> prepareBake(scene::Graph& g, const BakeRequest& request)
{
    LILA_ZONE();

    auto b = std::make_unique<PreparedBake>();
    b->started = Clock::now();
    scene::Node* p = g.node(request.project);
    if (!p || !isProject(*p))
        throw std::runtime_error("bake: no bake project");
    b->project = request.project;
    b->name = p->name();
    b->ps = projectSettings(*p);
    b->dryRun = request.dryRun;
    b->backend = createBackend(request.backend);
    if (!b->backend)
        throw std::runtime_error("bake: no backend named " + request.backend);
    auto t0 = Clock::now();
    b->inputs = buildInputs(g, request.project, request.groups, !request.dryRun);
    addTiming(b->timings, "inputs", since(t0));
    for (const auto& [k, v] : b->inputs.timings.items())
        addTiming(b->timings, k, v.get<double>());
    t0 = Clock::now();
    lintScene(g, b->inputs, b->sceneLint);
    addTiming(b->timings, "lint", since(t0));
    b->opt.maps = request.maps;
    b->opt.dryRun = request.dryRun;
    b->opt.folder = resolveFolder(b->ps.output.folder);
    if (!request.dryRun)
    {
        if (request.groups.empty() && request.maps.empty())
        {
            t0 = Clock::now();
            b->opt.inputHash = inputHash(g, request.project);
            addTiming(b->timings, "inputHash", since(t0));
        }
        b->opt.prepareWrite = prepareOutputs;
    }
    return b;
}

/** The work after the snapshot; reads nothing of the scene, so any thread. */
BakeResult executeBake(PreparedBake& b)
{
    LILA_ZONE();

    auto t0 = Clock::now();
    if (!b.dryRun)
    {
        b.backend->prepare(b.inputs);
        addTiming(b.timings, "accel", since(t0));
        t0 = Clock::now();
        resolveAutoDistances(b.inputs, *b.backend);
        addTiming(b.timings, "autoDistances", since(t0));
    }
    // Lint first, on the same inputs traced by the same backend, so its warnings lead the bake's.
    t0 = Clock::now();
    LintReport lintReport = lintInputs(b.inputs, b.ps, b.dryRun ? nullptr : b.backend.get());
    lintReport.issues.insert(lintReport.issues.end(), b.sceneLint.issues.begin(), b.sceneLint.issues.end());
    addTiming(b.timings, "lint", since(t0));
    BakeResult r = runBake(b.inputs, b.ps, b.opt, *b.backend);
    r.backend = b.backend->name();
    r.backendMode = b.backend->mode();
    for (const auto& [k, v] : b.timings.items())
        addTiming(r.timings, k, v.get<double>());
    const std::vector<std::string> lintLines = lintReport.lines();
    r.warnings.insert(r.warnings.begin(), lintLines.begin(), lintLines.end());
    r.lint = lintReport.toJson();
    if (!b.dryRun)
        addTiming(r.timings, "total", since(b.started));
    return r;
}

std::mutex gListenerMutex;
std::map<int, std::function<void(const BakeEvent&)>>& listeners()
{
    static std::map<int, std::function<void(const BakeEvent&)>> l;
    return l;
}

/** Tells the listeners, on the main thread (now when already there). */
void notify(BakeEvent e)
{
    auto send = [e = std::move(e)] {
        std::vector<std::function<void(const BakeEvent&)>> fns;
        {
            std::scoped_lock lock(gListenerMutex);
            for (const auto& [id, fn] : listeners())
                fns.push_back(fn);
        }
        for (const auto& fn : fns)
            fn(e);
    };
    if (mainq::isMainThread())
        send();
    else
        mainq::post(std::move(send));
}

/** Main thread, once the files are written: source control, the session's last result, the log, the listeners. */
void completeBake(scene::NodeId project, const std::string& name, const BakeResult& r)
{
    if (r.dryRun)
        return;
    std::vector<fs::path> written;
    for (const OutputFile& o : r.outputs)
        if (std::ranges::find(written, o.file) == written.end())
            written.push_back(o.file);
    addOutputs(written);
    json j = r.toJson();
    lastResults()[project] = j;
    uint64_t misses = 0, samples = 0;
    for (const GroupStats& s : r.groups)
    {
        misses += s.misses;
        samples += s.samples;
    }
    log::info("Baked {} texture(s) for {} in {:.2f} s on {} ({} of {} samples missed)", r.outputs.size(), name, r.seconds, r.backendMode, misses, samples);
    notify({BakeEvent::Kind::Finished, project, std::move(j)});
}

/** Written textures reach the listeners as they land, so previews fill in during the bake. */
void reportWrites(PreparedBake& b)
{
    if (b.dryRun)
        return;
    b.opt.onWritten = [project = b.project](const OutputFile& o) { notify({BakeEvent::Kind::Texture, project, o.toJson()}); };
}

struct AsyncBake
{
    std::mutex mutex;
    /** Joined by the next bakeAsync or at shutdown; a finished one may wait unjoined until then. */
    std::thread thread;
    bool running = false;
    std::string title;

    ~AsyncBake()
    {
        if (thread.joinable())
            thread.detach();
    }
};

AsyncBake& asyncBake()
{
    static AsyncBake a;
    return a;
}
} // namespace

BakeResult bake(scene::Graph& g, const BakeRequest& request)
{
    LILA_ZONE();

    std::unique_ptr<PreparedBake> b = prepareBake(g, request);
    reportWrites(*b);
    BakeResult r = executeBake(*b);
    completeBake(b->project, b->name, r);
    return r;
}

void bakeAsync(scene::Graph& g, const BakeRequest& request)
{
    LILA_ZONE();

    AsyncBake& a = asyncBake();
    {
        std::scoped_lock lock(a.mutex);
        if (a.running)
            throw std::runtime_error("bake: a bake is already running (bakeCancel stops it)");
    }
    if (a.thread.joinable())
        a.thread.join();
    std::shared_ptr<PreparedBake> b = prepareBake(g, request);
    reportWrites(*b);
    {
        std::scoped_lock lock(a.mutex);
        a.running = true;
        a.title = "Baking " + b->name;
    }
    notify({BakeEvent::Kind::Started, b->project, {{"project", b->name}, {"backend", request.backend}}});
    a.thread = std::thread([b] {
        json failure;
        std::shared_ptr<BakeResult> result;
        try
        {
            progress::Scope scope("Baking " + b->name, 1.0, progress::Cancellable);
            result = std::make_shared<BakeResult>(executeBake(*b));
        }
        catch (const progress::Cancelled&)
        {
            failure = {{"cancelled", true}, {"error", "cancelled"}};
        }
        catch (const std::exception& e)
        {
            failure = {{"error", e.what()}};
        }
        AsyncBake& a = asyncBake();
        std::scoped_lock lock(a.mutex);
        a.running = false;
        // Under the lock: whoever sees running false finds the completion queued, and a Finished listener can start the next bake.
        mainq::post([b, result, failure] {
            if (result)
            {
                completeBake(b->project, b->name, *result);
                return;
            }
            log::warn("Bake of {} {}", b->name, failure.value("cancelled", false) ? "cancelled" : "failed: " + failure.value("error", std::string()));
            json f = failure;
            f["project"] = b->name;
            notify({BakeEvent::Kind::Finished, b->project, std::move(f)});
        });
    });
}

bool bakeRunning()
{
    AsyncBake& a = asyncBake();
    std::scoped_lock lock(a.mutex);
    return a.running;
}

void cancelAsyncBake(bool wait)
{
    AsyncBake& a = asyncBake();
    std::string title;
    {
        std::scoped_lock lock(a.mutex);
        if (!a.running && !a.thread.joinable())
            return;
        title = a.title;
    }
    for (const progress::Snapshot& snap : progress::snapshot())
        if (snap.title == title && !snap.mainThread)
            progress::cancel(snap.id);
    if (wait && a.thread.joinable())
        a.thread.join();
}

int addBakeListener(std::function<void(const BakeEvent&)> fn)
{
    static int next = 1;
    std::scoped_lock lock(gListenerMutex);
    listeners()[next] = std::move(fn);
    return next++;
}

void removeBakeListener(int id)
{
    std::scoped_lock lock(gListenerMutex);
    listeners().erase(id);
}

json lastResult(scene::NodeId project)
{
    auto it = lastResults().find(project);
    return it == lastResults().end() ? json() : it->second;
}

std::vector<AutoGroup> matchAutoGroups(const std::vector<std::string>& names, std::vector<std::string>* unmatched)
{
    auto lower = [](std::string s) {
        for (char& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto endsWith = [](const std::string& s, const std::string& suffix) { return s.size() > suffix.size() && s.ends_with(suffix); };
    std::vector<AutoGroup> groups;
    auto groupOf = [&](const std::string& base) -> AutoGroup& {
        auto it = std::ranges::find_if(groups, [&](const AutoGroup& a) { return a.base == base; });
        if (it != groups.end())
            return *it;
        groups.push_back({base, {}, {}});
        return groups.back();
    };
    std::vector<std::string> rest;
    for (const std::string& name : names)
    {
        const std::string l = lower(name);
        bool matched = false;
        for (const char* suffix : {"_low", "_lo", "_lp"})
            if (endsWith(l, suffix))
            {
                groupOf(name.substr(0, name.size() - std::strlen(suffix))).lows.push_back(name);
                matched = true;
                break;
            }
        if (matched)
            continue;
        // Highs may carry a number: _high1, _high_2.
        size_t end = l.size();
        while (end > 0 && std::isdigit(static_cast<unsigned char>(l[end - 1])))
            --end;
        if (end < l.size() && end > 0 && l[end - 1] == '_')
            --end;
        const std::string stem = l.substr(0, end);
        for (const char* suffix : {"_high", "_hi", "_hp"})
            if (endsWith(stem, suffix))
            {
                groupOf(name.substr(0, end - std::strlen(suffix))).highs.push_back(name);
                matched = true;
                break;
            }
        if (!matched)
            rest.push_back(name);
    }
    std::vector<AutoGroup> out;
    for (AutoGroup& a : groups)
    {
        if (!a.lows.empty() && !a.highs.empty())
            out.push_back(std::move(a));
        else
        {
            rest.insert(rest.end(), a.lows.begin(), a.lows.end());
            rest.insert(rest.end(), a.highs.begin(), a.highs.end());
        }
    }
    if (unmatched)
        *unmatched = std::move(rest);
    return out;
}
} // namespace lila::bake
