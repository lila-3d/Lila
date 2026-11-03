#pragma once

#include <doctest/doctest.h>

#include "extensions/ExtensionManager.h"
#include "ops/Command.h"

namespace foliagetest
{
using namespace lila;
using json = nlohmann::json;

inline json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

inline void ensureLoaded()
{
    auto& em = extensions::ExtensionManager::get();
    if (!em.isLoaded("Foliage"))
        REQUIRE(em.load(LILA_TEST_FOLIAGE_PACKAGE) == "Foliage");
}

inline void fresh()
{
    ensureLoaded();
    run("newScene");
}
} // namespace foliagetest
