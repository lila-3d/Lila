// SPDX-License-Identifier: Apache-2.0
#pragma once

// The loaders' schema version check, on parsed JSON (schema.h).

#include <nlohmann/json.hpp>

#include "groomlab/schema.h"

namespace groomlab::detail {

// The document's "version" (1 when it has none). Throws SchemaError unless
// `doc` is an object whose version is a whole number this build reads: the
// current one (older ones are refused, there are no migrations).
int checkVersion(DocumentKind kind, const nlohmann::json& doc);

}  // namespace groomlab::detail
