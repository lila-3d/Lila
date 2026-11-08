#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::specs
{
/**
 * A key one extension adds to another's spec without either linking the other, such as the terrain spec's "seams"
 * from Seamwork. The spec's owner lists the steps registered for it in its schema and runs them after its own.
 * Registered under "<spec>.<key>".
 */
struct Step
{
    /** The spec it extends, such as "terrain". */
    std::string spec;
    std::string key;
    /** JSON schema of the key's value, for the spec's schema. */
    nlohmann::json schema;
    /** Builds the key's part on what the spec made (the terrain mesh's path) and returns what it made; throws on failure. */
    std::function<nlohmann::json(const std::string& target, const nlohmann::json& value)> run;
};

LILA_SCENE_API Registry<Step>& steps();

/** The steps extending spec, in key order. */
LILA_SCENE_API std::vector<const Step*> stepsFor(std::string_view spec);
} // namespace lila::specs
