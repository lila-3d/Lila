// SPDX-License-Identifier: Apache-2.0
#include "groomlab/schema.h"

#include <cstdint>
#include <string>

#include "schema_internal.h"

namespace groomlab {

namespace {

using json = nlohmann::json;

struct KindInfo {
    const char* name;
    int current;  // the version this build writes and reads; a document without a "version" is 1
};

// Formats change in place and the version is bumped only when a change would
// not read back, so an older document is refused rather than migrated (there
// are no migrations, by decision). Additions with defaults (the groom file's
// "units", its shortest-form floats) keep the version.
const KindInfo& info(DocumentKind kind) {
    static const KindInfo graph{"graph", 1};
    static const KindInfo guides{"guide data", 1};
    static const KindInfo groom{"groom file", 1};
    switch (kind) {
    case DocumentKind::Graph:
        return graph;
    case DocumentKind::GuideData:
        return guides;
    case DocumentKind::GroomFile:
    default:
        return groom;
    }
}

}  // namespace

namespace detail {

int checkVersion(DocumentKind kind, const json& doc) {
    const KindInfo& k = info(kind);
    const std::string what = k.name;
    if (!doc.is_object()) throw SchemaError(what + " must be a JSON object");
    if (!doc.contains("version")) return 1;
    const json& version = doc.at("version");
    if (!version.is_number_integer()) throw SchemaError(what + " version " + version.dump() + " is not a whole number");
    const std::int64_t stored = version.get<std::int64_t>();
    if (stored > k.current) {
        throw SchemaError(what + " version " + std::to_string(stored) + " is newer than this GroomLab supports (" +
                          std::to_string(k.current) + "); update GroomLab to open it");
    }
    if (stored < 1) throw SchemaError(what + " version " + std::to_string(stored) + " is not valid");
    if (stored < k.current) {
        throw SchemaError(what + " version " + std::to_string(stored) + " is older than this GroomLab reads (" +
                          std::to_string(k.current) + ")");
    }
    return static_cast<int>(stored);
}

}  // namespace detail

const char* documentKindName(DocumentKind kind) { return info(kind).name; }

int currentSchemaVersion(DocumentKind kind) { return info(kind).current; }

int schemaVersion(DocumentKind kind, const std::string& text) {
    json doc;
    try {
        doc = json::parse(text);
    } catch (const json::exception& e) {
        throw SchemaError(std::string(documentKindName(kind)) + ": invalid JSON: " + e.what());
    }
    return detail::checkVersion(kind, doc);
}

}  // namespace groomlab
