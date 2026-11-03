#pragma once

#include "ops/Command.h"

#include <filesystem>
#include <string>

namespace animtest
{
using namespace lila;
using json = nlohmann::json;

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
inline double get(const std::string& plug) { return run("getAttr", {{"args", {plug}}}).get<double>(); }
inline void at(double seconds) { run("currentTime", {{"args", {seconds}}}); }
inline bool exists(const std::string& name) { return run("objExists", {{"args", {name}}}).get<bool>(); }
inline json v3(double x, double y, double z) { return json::array({x, y, z}); }

inline std::string tempPath(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_anim_tests" / "scenes";
    std::filesystem::create_directories(dir);
    return (dir / name).generic_string();
}
} // namespace animtest
