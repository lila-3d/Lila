#pragma once

#include "gn/Gn.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <initializer_list>
#include <string>

namespace gntest
{
using namespace lila;
using json = nlohmann::json;

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
inline json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }

inline gn::Group group(const char* name) { return gn::readGroup(*gn::findGroupNode(scene::Graph::current(), name)); }
inline std::string add(const std::string& g, const char* type) { return run("gnAddNode", args({g, type})).get<std::string>(); }
inline void link(const std::string& g, const std::string& from, const std::string& to) { run("gnConnect", args({g, from, to})); }

inline int faces(const std::string& m) { return run("meshInfo", args({m}))["faces"].get<int>(); }
inline json bounds(const std::string& m) { return run("meshInfo", args({m})); }
} // namespace gntest
