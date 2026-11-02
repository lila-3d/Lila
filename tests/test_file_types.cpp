#include <doctest/doctest.h>

#include "io/FileTypes.h"

#include <fstream>

using namespace lila::io::fileTypes;

namespace
{
const fs::path kExe = "C:/Apps/Lila/lila.exe";
#ifdef _WIN32
const std::string kExePath = "C:\\Apps\\Lila\\lila.exe";
#else
const std::string kExePath = "C:/Apps/Lila/lila.exe";
#endif
const std::string kOpen = "\"" + kExePath + "\" \"%1\"";
} // namespace

TEST_CASE("file types: a scene format gets its ProgID, icon, open command and the double-click")
{
    MemoryRegistry r;
    CHECK(registerType(r, kExe, {".lila", "Lila scene", true}));
    CHECK(r.get("Lila.lila", "") == "Lila scene");
    CHECK(r.get("Lila.lila\\DefaultIcon", "") == kExePath + ",0");
    CHECK(r.get("Lila.lila\\shell\\open\\command", "") == kOpen);
    CHECK(r.get(".lila", "") == "Lila.lila");
    CHECK(r.get(".lila\\OpenWithProgids", "Lila.lila") == "");
    CHECK(r.get("Applications\\lila.exe\\SupportedTypes", ".lila") == "");
    CHECK(r.get("Applications\\lila.exe\\shell\\open\\command", "") == kOpen);
    CHECK(isRegistered(r, ".lila"));
    // Already in place: nothing to write, so Explorer isn't told again.
    CHECK_FALSE(registerType(r, kExe, {".lila", "Lila scene", true}));
}

TEST_CASE("file types: an import format taken over is handed back as it was")
{
    MemoryRegistry r;
    r.set(".fbx", "", "Autodesk.FBX");
    r.set(".fbx\\OpenWithProgids", "Blender.fbx", "");
    registerType(r, kExe, {".FBX", "FBX file", false});
    CHECK(r.get(".fbx", "") == "Lila.fbx");
    CHECK(r.get(".fbx\\OpenWithProgids", "Blender.fbx") == "");

    CHECK(unregisterType(r, kExe, ".fbx"));
    CHECK(r.get(".fbx", "") == "Autodesk.FBX");
    CHECK(r.get(".fbx\\OpenWithProgids", "Blender.fbx") == "");
    CHECK_FALSE(r.get(".fbx\\OpenWithProgids", "Lila.fbx"));
    CHECK_FALSE(r.get("Lila.fbx", ""));
    CHECK_FALSE(r.get("Lila.fbx\\shell\\open\\command", ""));
    CHECK_FALSE(isRegistered(r, ".fbx"));
    CHECK_FALSE(unregisterType(r, kExe, ".fbx"));
}

TEST_CASE("file types: unregistering leaves a default someone else set since")
{
    MemoryRegistry r;
    registerType(r, kExe, {".obj", "OBJ file", false});
    r.set(".obj", "", "Other.obj");
    unregisterType(r, kExe, ".obj");
    CHECK(r.get(".obj", "") == "Other.obj");
    CHECK_FALSE(r.get("Lila.obj", ""));
}

TEST_CASE("file types: sync registers scenes and only the ticked imports")
{
    MemoryRegistry r;
    r.set(".fbx", "", "Autodesk.FBX");
    sync(r, kExe, true, {});
    CHECK(isRegistered(r, ".lila"));
    CHECK(isRegistered(r, ".lilaa"));
    // Import formats untouched until ticked.
    CHECK(r.get(".fbx", "") == "Autodesk.FBX");
    CHECK_FALSE(r.get("Lila.fbx", ""));

    sync(r, kExe, false, {});
    CHECK_FALSE(isRegistered(r, ".lila"));
    CHECK_FALSE(r.get("Lila.lila", ""));
    CHECK_FALSE(r.get("Applications\\lila.exe", "FriendlyAppName"));
    CHECK(r.get(".fbx", "") == "Autodesk.FBX");
}

TEST_CASE("file types: source builds don't register scenes unless asked")
{
    const fs::path dir = fs::temp_directory_path() / "lila_tests" / "filetypes_src" / "bin" / "RelWithDebInfo";
    fs::create_directories(dir);
    { std::ofstream(dir.parent_path().parent_path() / "CMakeCache.txt") << "\n"; }
    CHECK_FALSE(scenesByDefault(dir));
    const fs::path installed = fs::temp_directory_path() / "lila_tests" / "filetypes_inst" / "Lila";
    fs::create_directories(installed);
    CHECK(scenesByDefault(installed));
}
