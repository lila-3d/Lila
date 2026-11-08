#include "link/TileSend.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "link/UnrealLink.h"

#include <cstdio>
#include <fstream>
#include <set>

namespace lila::link
{
namespace
{
fs::path manifestPath(const TileSendOptions& o)
{
    const fs::path dir = o.manifestDir.empty() ? cacheDir() / "manifests" : o.manifestDir;
    return dir / (o.manifest + ".json");
}

json readManifest(const fs::path& p)
{
    std::ifstream in(p);
    if (!in)
        return json::object();
    json j = json::parse(in, nullptr, false);
    return j.is_object() && j.contains("tiles") && j["tiles"].is_object() ? j["tiles"] : json::object();
}

void writeManifest(const fs::path& p, const json& tiles)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    // Written beside and renamed, so a send killed mid-write never leaves a broken manifest.
    const fs::path tmp = fs::path(p).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << json{{"tiles", tiles}}.dump(1);
    }
    fs::rename(tmp, p, ec);
}

std::string hex(uint64_t h)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}
} // namespace

uint64_t hashBytes(const void* data, size_t size, uint64_t seed)
{
    const auto* b = static_cast<const unsigned char*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

TileSendResult sendTiles(RpcClient& client, const std::vector<TileItem>& items, const TileSendOptions& opts)
{
    LILA_ZONE();
    TileSendResult r;
    r.assets.resize(items.size());
    const fs::path path = manifestPath(opts);
    json tiles = readManifest(path);

    std::set<std::string> keys;
    for (const TileItem& it : items)
        keys.insert(it.key);
    for (auto it = tiles.begin(); it != tiles.end();)
    {
        if (!keys.count(it.key()))
        {
            r.stale.push_back(it.key());
            it = tiles.erase(it);
        }
        else
            ++it;
    }
    if (!r.stale.empty())
        writeManifest(path, tiles);

    int sinceCollect = 0;
    for (size_t i = 0; i < items.size(); ++i)
    {
        const TileItem& item = items[i];
        if (opts.progress && !opts.progress(i, items.size(), item.key))
        {
            r.cancelled = true;
            break;
        }
        if (!opts.force && tiles.contains(item.key) && tiles[item.key].value("hash", std::string()) == hex(item.hash))
        {
            r.assets[i] = tiles[item.key].value("asset", std::string());
            ++r.skipped;
            continue;
        }
        try
        {
            const fs::path file = item.write();
            RpcResult imp = client.call("importAsset", importParams(file, item.mapping), importCallOptions());
            if (opts.deleteFiles)
            {
                std::error_code ec;
                fs::remove(file, ec);
            }
            if (!imp.ok)
                throw std::runtime_error("importAsset: " + imp.error);
            // The import also lists materials it made; the tile is the asset at its content path.
            const std::string contentPath = item.mapping.value("contentPath", std::string());
            const std::string name = contentPath.substr(contentPath.find_last_of('/') + 1);
            std::string asset = contentPath + "." + name;
            for (const json& a : imp.result.value("assets", json::array()))
                if (a.is_string() && a.get<std::string>().ends_with("/" + name + "." + name))
                    asset = a.get<std::string>();
            if (item.after)
                item.after(asset);
            r.assets[i] = asset;
            tiles[item.key] = {{"hash", hex(item.hash)}, {"asset", asset}};
            writeManifest(path, tiles);
            ++r.sent;
        }
        catch (const std::exception& e)
        {
            ++r.failed;
            r.errors.push_back(item.key + ": " + e.what());
            log::warn("sendTiles: {}: {}", item.key, e.what());
        }
        if (opts.collectEvery > 0 && ++sinceCollect >= opts.collectEvery)
        {
            sinceCollect = 0;
            client.call("collectGarbage", json::object(), 60000);
        }
    }
    return r;
}
} // namespace lila::link
