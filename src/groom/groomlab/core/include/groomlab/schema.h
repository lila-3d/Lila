// SPDX-License-Identifier: Apache-2.0
#pragma once

// Schema versions for everything GroomLab stores: graphs (graph attribute,
// presets), guide data (guideData attribute) and groom files. A document of
// another version than the build's is refused with a clear error; there are no
// migrations (by decision: formats change in place, unknown fields are
// ignored on load, and the version is bumped only when an old document would
// not read back at all). The compatibility tests load the fixtures under
// tests/data/compat to catch unintended format changes; update them with an
// intended one.

#include <stdexcept>
#include <string>

namespace groomlab {

enum class DocumentKind { Graph, GuideData, GroomFile };

class SchemaError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

const char* documentKindName(DocumentKind kind);
int currentSchemaVersion(DocumentKind kind);

// The stored version of a document (JSON text; 1 without a "version").
// Throws SchemaError for invalid JSON, a version that is not a whole number,
// or one this build does not read (newer, or older than the current one).
int schemaVersion(DocumentKind kind, const std::string& text);

}  // namespace groomlab
