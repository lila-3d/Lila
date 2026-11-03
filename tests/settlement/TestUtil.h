#pragma once

#include "ops/Command.h"

namespace settlementtest
{
using json = nlohmann::json;

inline json run(const char* name, json call = json::object()) { return lila::ops::execute(name, call); }
} // namespace settlementtest
