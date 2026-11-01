#include "brush/Alphas.h"
#include "brush/Brush.h"
#include "core/Paths.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>

using namespace lila;
namespace alphas = brush::alphas;
namespace fs = std::filesystem;

TEST_CASE("the alpha library lists Lila's alphas and resolves references to them")
{
    const std::vector<alphas::Entry> lib = alphas::library();
    auto ring = std::ranges::find_if(lib, [](const alphas::Entry& e) { return e.ref == "alpha://lila/shapes/ring.png"; });
    REQUIRE(ring != lib.end());
    CHECK(ring->name == "ring");
    CHECK(ring->category == "shapes");
    CHECK(fs::path(alphas::resolve(ring->ref)) == ring->file);
    CHECK(alphas::displayName(ring->ref) == "ring");
    CHECK(alphas::resolve("alpha://lila/shapes/nothing.png").empty());
    CHECK(alphas::resolve("C:/some/file.png") == "C:/some/file.png");

    brush::BrushSettings s;
    s.alphaPath = ring->ref;
    s.ensureAlpha();
    REQUIRE(s.alpha);
    CHECK(s.alpha->width == 256);
}

TEST_CASE("adding an alpha copies it into the user's folder under a free name")
{
    const fs::path user = alphas::userDir();
    // main() points the config folder at a temp dir; never clear a real one.
    REQUIRE(paths::toUtf8(user).find("lila_sculpt_tests") != std::string::npos);
    std::error_code ec;
    fs::remove_all(user, ec);
    const fs::path src = paths::resourcesDir() / "alphas" / "stone" / "pebbles.png";
    const std::string first = alphas::add(src, user);
    const std::string second = alphas::add(fs::path(alphas::resolve(first)), user);
    CHECK(first == "alpha://user/pebbles.png");
    CHECK(second == first);
    fs::path copy = fs::temp_directory_path() / "lila_alpha_copy" / "pebbles.png";
    fs::create_directories(copy.parent_path(), ec);
    fs::copy_file(src, copy, fs::copy_options::overwrite_existing, ec);
    CHECK(alphas::add(copy, user) == "alpha://user/pebbles_2.png");
    const std::vector<alphas::Entry> lib = alphas::library();
    CHECK(std::ranges::count_if(lib, [](const alphas::Entry& e) { return e.source == "user"; }) == 2);
    fs::remove_all(user, ec);
}
