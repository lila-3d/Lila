#include <doctest/doctest.h>

#include "core/PrefStore.h"
#include "ui/HotkeyChord.h"
#include "ui/Keymaps.h"

#include <filesystem>
#include <fstream>

using namespace lila;
using namespace lila::ui;
using nlohmann::json;
namespace fs = std::filesystem;

TEST_CASE("keys match by position only for the number row unless the pref says positions")
{
    CHECK(hotkeys::matchesByPosition(0x02, false)); // 1
    CHECK(hotkeys::matchesByPosition(0x0B, false)); // 0
    CHECK_FALSE(hotkeys::matchesByPosition(0x2C, false)); // Z on US, Y on QWERTZ
    CHECK_FALSE(hotkeys::matchesByPosition(0x15, false));
    CHECK(hotkeys::matchesByPosition(0x2C, true));
    CHECK_FALSE(hotkeys::matchesByPosition(0x3B, true)); // F1 matches by key either way
}

TEST_CASE("keymaps: switch, copy, rename, delete, export and import")
{
    PrefStore& prefs = PrefStore::get();
    std::error_code ec;
    fs::remove_all(prefs.userDir() / "keymaps", ec);
    prefs.remove("general", keymaps::kActivePref);
    const json undoKey = {{"contexts", {{"global", {{"Ctrl+Z", "edit.undo"}}}}}};
    const json mine = {{"contexts", {{"global", {{"Ctrl+Y", "edit.undo"}}}}}};
    prefs.setUserDoc("hotkeys", undoKey);

    CHECK(keymaps::active() == "Default");
    keymaps::create("Mine", true);
    CHECK(keymaps::active() == "Mine");
    CHECK(prefs.userDoc("hotkeys").empty());
    prefs.setUserDoc("hotkeys", mine);

    // Switching keeps each keymap's own edits.
    keymaps::use("Default");
    CHECK(prefs.userDoc("hotkeys") == undoKey);
    keymaps::use("Mine");
    CHECK(prefs.userDoc("hotkeys") == mine);

    keymaps::create("Copy");
    CHECK(prefs.userDoc("hotkeys") == mine);
    CHECK_THROWS_AS(keymaps::create("Copy"), std::invalid_argument);
    CHECK_THROWS_AS(keymaps::create("a/b"), std::invalid_argument);

    keymaps::rename("Copy", "Renamed");
    CHECK(keymaps::active() == "Renamed");
    std::vector<std::string> names;
    for (const keymaps::Info& k : keymaps::list())
        names.push_back(k.name);
    CHECK(names == std::vector<std::string>{"Default", "Mine", "Renamed"});

    const fs::path file = prefs.userDir() / "shared.lilakeys";
    keymaps::exportTo("Mine", file);
    CHECK(keymaps::importFrom(file) == "Mine 2");
    keymaps::use("Mine 2");
    CHECK(prefs.userDoc("hotkeys") == mine);

    // A bare hotkeys.json imports too, named after the file.
    const fs::path bare = prefs.userDir() / "Studio Keys.json";
    std::ofstream(bare) << undoKey.dump();
    CHECK(keymaps::importFrom(bare) == "Studio Keys");

    CHECK(keymaps::uniqueName("Mine") == "Mine 3");
    keymaps::reset("Mine 2");
    CHECK(prefs.userDoc("hotkeys").empty());
    keymaps::use("Mine");
    CHECK(prefs.userDoc("hotkeys") == mine);
    keymaps::use("Mine 2");

    keymaps::remove("Mine 2");
    CHECK(keymaps::active() == "Default");
    CHECK(prefs.userDoc("hotkeys") == undoKey);
    CHECK_THROWS_AS(keymaps::remove("Default"), std::invalid_argument);
    CHECK_THROWS_AS(keymaps::use("Mine 2"), std::invalid_argument);

    fs::remove_all(prefs.userDir() / "keymaps", ec);
    prefs.remove("general", keymaps::kActivePref);
    prefs.setUserDoc("hotkeys", json::object());
}
