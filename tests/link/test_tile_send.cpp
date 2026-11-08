#include "FakeServer.h"

#include "link/RpcClient.h"
#include "link/TileSend.h"

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

using namespace lila::link;
using json = nlohmann::json;

namespace
{
FakeServer::Handler importer(std::atomic<int>& imports, std::atomic<int>& collects, const std::string& failOn = {})
{
    return [&imports, &collects, failOn](const json& req) -> std::optional<json> {
        const std::string m = req.value("method", "");
        if (m == "importAsset")
        {
            const std::string path = req["params"].value("contentPath", "");
            if (!failOn.empty() && path.ends_with(failOn))
                return json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"error", {{"code", -32000}, {"message", "import failed"}}}};
            ++imports;
            const std::string name = path.substr(path.find_last_of('/') + 1);
            return FakeServer::reply(req, {{"assets", json::array({"/Game/M_Made.M_Made", path + "." + name})}});
        }
        if (m == "collectGarbage")
        {
            ++collects;
            return FakeServer::reply(req, true);
        }
        return FakeServer::reply(req, nullptr);
    };
}

std::vector<TileItem> tiles(const fs::path& dir, int n, uint64_t salt = 0)
{
    std::vector<TileItem> out;
    for (int i = 0; i < n; ++i)
    {
        TileItem t;
        t.key = "cliff/x" + std::to_string(i);
        t.hash = hashBytes(&i, sizeof(i), 1469598103934665603ull + salt * (i == 1));
        t.mapping = {{"contentPath", "/Game/Cliffs/SM_Tile_" + std::to_string(i)}, {"kind", "static"}};
        const fs::path file = dir / ("tile" + std::to_string(i) + ".fbx");
        t.write = [file] {
            std::ofstream(file) << "fbx";
            return file;
        };
        out.push_back(std::move(t));
    }
    return out;
}

fs::path scratch(const char* name)
{
    const fs::path d = fs::temp_directory_path() / "lila_tile_send" / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
} // namespace

TEST_CASE("sendTiles: sends once, skips unchanged, resends changed, drops stale")
{
    std::atomic<int> imports{0}, collects{0};
    FakeServer server(importer(imports, collects));
    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));
    const fs::path dir = scratch("basic");
    TileSendOptions o;
    o.manifest = "cliffs";
    o.manifestDir = dir;
    o.collectEvery = 2;

    std::vector<std::string> placed;
    std::vector<TileItem> items = tiles(dir, 4);
    for (TileItem& t : items)
        t.after = [&placed](const std::string& a) { placed.push_back(a); };
    TileSendResult r = sendTiles(client, items, o);
    CHECK(r.sent == 4);
    CHECK(r.failed == 0);
    CHECK(imports == 4);
    CHECK(collects == 2);
    CHECK(r.assets[2] == "/Game/Cliffs/SM_Tile_2.SM_Tile_2");
    CHECK(placed.size() == 4);
    CHECK_FALSE(fs::exists(dir / "tile0.fbx"));

    r = sendTiles(client, tiles(dir, 4), o);
    CHECK(r.sent == 0);
    CHECK(r.skipped == 4);
    CHECK(r.assets[3] == "/Game/Cliffs/SM_Tile_3.SM_Tile_3");

    // Tile 1 changed, tile 3 is gone.
    r = sendTiles(client, tiles(dir, 3, 7), o);
    CHECK(r.sent == 1);
    CHECK(r.skipped == 2);
    REQUIRE(r.stale.size() == 1);
    CHECK(r.stale[0] == "cliff/x3");
    CHECK(imports == 5);

    o.force = true;
    r = sendTiles(client, tiles(dir, 3, 7), o);
    CHECK(r.sent == 3);
}

TEST_CASE("sendTiles: a cancelled send resumes where it stopped; a failed tile doesn't stop the rest")
{
    std::atomic<int> imports{0}, collects{0};
    FakeServer server(importer(imports, collects, "SM_Tile_2"));
    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));
    const fs::path dir = scratch("resume");
    TileSendOptions o;
    o.manifest = "cliffs";
    o.manifestDir = dir;
    o.collectEvery = 0;
    o.progress = [](size_t i, size_t, const std::string&) { return i < 2; };

    TileSendResult r = sendTiles(client, tiles(dir, 5), o);
    CHECK(r.cancelled);
    CHECK(r.sent == 2);

    o.progress = nullptr;
    r = sendTiles(client, tiles(dir, 5), o);
    CHECK_FALSE(r.cancelled);
    CHECK(r.skipped == 2);
    CHECK(r.sent == 2);
    CHECK(r.failed == 1);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].find("cliff/x2") == 0);
    CHECK(r.assets[2].empty());
    CHECK(collects == 0);

    // The failed tile is retried next time; the rest are skipped.
    r = sendTiles(client, tiles(dir, 5), o);
    CHECK(r.skipped == 4);
    CHECK(r.failed == 1);
}

TEST_CASE("sendTiles: unchanged tiles Unreal lost are imported or placed again")
{
    std::mutex m;
    std::set<std::string> assets, groups;
    std::atomic<int> imports{0};
    FakeServer server([&](const json& req) -> std::optional<json> {
        std::scoped_lock lock(m);
        const std::string method = req.value("method", "");
        const json& p = req["params"];
        if (method == "importAsset")
        {
            ++imports;
            const std::string path = p.value("contentPath", "");
            const std::string asset = path + "." + path.substr(path.find_last_of('/') + 1);
            assets.insert(asset);
            return FakeServer::reply(req, {{"assets", json::array({asset})}});
        }
        if (method == "listAssets")
        {
            json out = json::array();
            for (const std::string& a : assets)
                if (a.starts_with(p.value("path", "") + "/"))
                    out.push_back({{"path", a}, {"class", "StaticMesh"}});
            return FakeServer::reply(req, out);
        }
        if (method == "listPlacements")
        {
            json out = json::array();
            for (const json& g : p["groups"])
                if (groups.count(g.get<std::string>()))
                    out.push_back({{"group", g}, {"actor", g}});
            return FakeServer::reply(req, out);
        }
        return FakeServer::reply(req, nullptr);
    });
    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));
    const fs::path dir = scratch("verify");
    TileSendOptions o;
    o.manifest = "cliffs";
    o.manifestDir = dir;
    o.collectEvery = 0;
    int placements = 0;
    auto items = [&] {
        std::vector<TileItem> out = tiles(dir, 3);
        for (TileItem& t : out)
        {
            t.group = t.key;
            t.after = [&, key = t.key](const std::string&) {
                std::scoped_lock lock(m);
                ++placements;
                groups.insert(key);
            };
        }
        return out;
    };

    TileSendResult r = sendTiles(client, items(), o);
    CHECK(r.sent == 3);
    CHECK(placements == 3);
    r = sendTiles(client, items(), o);
    CHECK(r.skipped == 3);
    CHECK(r.warnings.empty());

    {
        std::scoped_lock lock(m);
        assets.erase("/Game/Cliffs/SM_Tile_0.SM_Tile_0");
        groups.erase("cliff/x0");
        groups.erase("cliff/x2");
    }
    r = sendTiles(client, items(), o);
    CHECK(r.restored == 1);
    CHECK(r.sent == 1);
    CHECK(r.replaced == 1);
    CHECK(r.skipped == 1);
    CHECK(imports == 4);
    CHECK(placements == 5);

    r = sendTiles(client, items(), o);
    CHECK(r.skipped == 3);
}

TEST_CASE("sendTiles: an Unreal that can't list assets skips unchanged tiles with a warning")
{
    std::atomic<int> imports{0}, collects{0};
    FakeServer server(importer(imports, collects));
    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));
    const fs::path dir = scratch("noverify");
    TileSendOptions o;
    o.manifest = "cliffs";
    o.manifestDir = dir;
    o.collectEvery = 0;
    sendTiles(client, tiles(dir, 2), o);
    const TileSendResult r = sendTiles(client, tiles(dir, 2), o);
    CHECK(r.skipped == 2);
    CHECK(r.warnings.size() == 1);
}
