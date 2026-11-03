#pragma once

#include <doctest/doctest.h>

#include "scene/Graph.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mayatest
{
using namespace lila;

inline std::filesystem::path fixture(const std::string& name) { return std::filesystem::path(LILA_MAYA_TEST_DATA) / name; }

inline bool contains(const std::vector<std::string>& lines, std::string_view what)
{
    for (const std::string& s : lines)
        if (s.find(what) != std::string::npos)
            return true;
    return false;
}

inline scene::Node* node(scene::Graph& g, const std::string& name)
{
    scene::Node* n = g.find(name);
    if (!n)
        MESSAGE("missing node " << name);
    REQUIRE(n);
    return n;
}

inline scene::Node* node(const std::string& name) { return node(scene::Graph::current(), name); }
} // namespace mayatest
