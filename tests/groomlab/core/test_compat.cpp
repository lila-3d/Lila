// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Stored-format compatibility: the version check (there are no migrations),
// and v1 fixtures of every stored document that must keep loading, and evaluating the same, in
// every later release. Formats change in place, so an intended format change
// updates the fixtures (tools/make_compat_fixtures.py) with it. expected.json records the
// evaluation and is re-recorded when an operator's output changes on purpose
// (Interpolate's weights, say).

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv, read-only
#endif

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/random.h"
#include "groomlab/schema.h"

namespace groomlab {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

const fs::path kCompat = fs::path(GROOMLAB_TEST_DATA) / "compat";

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

TEST(Schema, EveryKindRefusesNewerAndInvalidVersions) {
    for (const DocumentKind kind : {DocumentKind::Graph, DocumentKind::GuideData, DocumentKind::GroomFile}) {
        const int current = currentSchemaVersion(kind);
        EXPECT_THROW(schemaVersion(kind, R"({"version": )" + std::to_string(current + 1) + "}"), SchemaError)
            << documentKindName(kind);
        EXPECT_EQ(schemaVersion(kind, R"({"version": )" + std::to_string(current) + "}"), current);
        EXPECT_EQ(schemaVersion(kind, "{}"), 1);  // unversioned: 1
        // Not a whole number: a SchemaError, not a JSON type error escaping the loaders.
        for (const char* bad : {R"({"version": "1"})", R"({"version": 1.5})", R"({"version": null})", R"({"version": 0})",
                                R"({"version": -3})", R"({"version": 1e30})"}) {
            EXPECT_THROW(schemaVersion(kind, bad), SchemaError) << documentKindName(kind) << " " << bad;
        }
    }
    EXPECT_THROW(schemaVersion(DocumentKind::Graph, "[1, 2]"), SchemaError);
    EXPECT_THROW(schemaVersion(DocumentKind::Graph, "{"), SchemaError);

    // The loaders refuse them with their own errors.
    EXPECT_THROW(loadGraphJson(R"({"version": "one", "nodes": [], "output": "x"})"), GraphError);
    EXPECT_THROW(loadGraphJson(R"({"version": 99, "nodes": [], "output": "x"})"), GraphError);
    EXPECT_THROW(GuideHierarchy::fromJson(R"({"version": 2.5, "guides": []})"), std::runtime_error);
    EXPECT_THROW(groomFileFromJson(R"({"format": "groomlab.groom", "version": "2"})", ""), GroomFileError);
}

// v1 fixtures: a graph using every operator with maps and ramps, a hierarchy
// with locked children and attributes, and a groom file holding both.
struct Expected {
    std::size_t strands;
    std::string idsHash;
    float firstTipX;
};

std::uint64_t idsHash(const StrandSet& s) {
    std::uint64_t h = hash64(s.strandCount());
    for (const StrandId id : s.ids()) h = hashCombine(h, id);
    return h;
}

bool updating() {
    const char* v = std::getenv("GROOMLAB_UPDATE_GOLDEN");
    return v && *v && std::string(v) != "0";
}

}  // namespace
}  // namespace groomlab
