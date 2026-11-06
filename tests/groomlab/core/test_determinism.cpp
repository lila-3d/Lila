// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Determinism: a groom never changes with how it is evaluated. Every shipped preset on the
// reference groom (the PresetLooks one: tests/data/strokes/strokes.groom, the preset fitted to
// its size) gives the same strands bit for bit (IDs, roots, points, widths, every channel, the
// instances, the cards and the feathers' shafts) in a fresh session or after other grooms were
// evaluated, on one thread or all of them, and after its groom file is written and read back.
// The C API's own threads give the same again (tests/engine/capi/test_capi_wrapper.cpp), and so
// do the time-dependent nodes cold at a frame and played to it (tests/engine/io/test_time_determinism.cpp).

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv, read-only
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#if GROOMLAB_HAS_TBB
#include <tbb/global_control.h>
#endif

#include "groomlab/cards.h"
#include "groomlab/feathers.h"
#include "groomlab/groom_file.h"
#include "groomlab/instances.h"
#include "groomlab/parallel.h"
#include "groomlab/paths.h"
#include "groomlab/portable_math.h"
#include "groomlab/presets.h"
#include "groomlab/random.h"
#include "groomlab/scalp_mesh.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

const std::string kShipped = GROOMLAB_SHIPPED_PRESETS;
constexpr float kDensity = 0.05f;  // PresetLooks': thousands of strands, so every parallel loop splits

// Bytes of a vector of plain values (no padding), into a hash.
template <class T>
std::uint64_t hashArray(std::uint64_t h, const std::vector<T>& values) {
    static_assert(std::is_trivially_copyable<T>::value, "plain values only");
    h = hashCombine(h, values.size());
    const auto* bytes = reinterpret_cast<const unsigned char*>(values.data());
    const std::size_t size = values.size() * sizeof(T);
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, bytes + i, 8);
        h = hashCombine(h, word);
    }
    for (; i < size; ++i) h = hashCombine(h, bytes[i]);
    return h;
}

// Everything a groom's evaluation hands a host, hashed: the strands (StrandSet::contentHash: IDs,
// roots, offsets, points, widths, channels, instances), the cards and the feathers' shafts.
std::uint64_t everything(const StrandSet& s) {
    std::uint64_t h = s.contentHash();
    h = hashString(h, "channels");
    for (const std::string& name : s.strandChannelNames()) h = hashCombine(hashString(h, name), hashFloat(0, s.strandChannelDefault(name)));
    for (const std::string& name : s.pointChannelNames()) h = hashCombine(hashString(h, name), hashFloat(0, s.pointChannelDefault(name)));
    for (const auto& cards : s.cardLayers()) {
        if (!cards) continue;
        h = hashArray(h, cards->positions);
        h = hashArray(h, cards->normals);
        h = hashArray(h, cards->u);
        h = hashArray(h, cards->v);
        h = hashArray(h, cards->tints);
        h = hashArray(h, cards->iridescence);
        h = hashArray(h, cards->quads);
        for (const CardMesh::Card& card : cards->cards) {
            h = hashCombine(h, card.firstVertex);
            h = hashCombine(h, static_cast<std::uint64_t>(card.stations));
            for (const float c : card.cell) h = hashFloat(h, c);
            h = hashArray(h, card.strands);
            h = hashArray(h, card.width);
        }
    }
    for (const auto& layer : s.featherLayers()) {
        if (!layer) continue;
        h = hashString(h, layer->node);
        h = hashString(h, layer->groupName);
        h = hashCombine(h, static_cast<std::uint64_t>(layer->groupId));
        h = hashArray(h, layer->offsets);
        h = hashArray(h, layer->rings);
        h = hashArray(h, layer->featherIds);
    }
    return h;
}

struct Preset {
    std::string name;
    std::string graph;  // fitted to the reference groom's scalp
    std::string text;   // the preset file's (what it requires)
};

// The reference groom and every shipped preset's graph fitted to it.
struct Reference {
    GroomFile file;
    std::vector<Preset> presets;

    Reference() : file(readGroomFile(std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom")) {
        const std::optional<double> size = presetScalpSize(*file.data.scalp);
        // The shipped presets, and the test's own grooms of the nodes no preset uses yet
        // (tests/data/presets: parity_nodes, every node and param of batch N2).
        for (const PresetInfo& p : listPresets({kShipped, std::string(GROOMLAB_TEST_DATA) + "/presets"})) {
            std::ifstream in(std::filesystem::u8path(p.path), std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            presets.push_back({p.name, presetGraphJson(text.str(), size), text.str()});
        }
    }
};

const Reference& reference() {
    static const Reference r;
    return r;
}

// The reference groom with the preset's graph and the maps it requires (made on applying it), painted.
GroomData dataOf(const Preset& preset) {
    GroomData data = reference().file.data;
    data.graphJson = preset.graph;
    test::paintRequiredMaps(preset.text, data);
    return data;
}

// What one evaluation made: its hash and size, or its error.
struct Made {
    std::uint64_t hash = 0;
    std::size_t strands = 0;
    std::string error;

    bool operator==(const Made& o) const { return hash == o.hash && strands == o.strands && error == o.error; }
};

std::ostream& operator<<(std::ostream& out, const Made& m) {
    return out << (m.error.empty() ? "" : "error '" + m.error + "' ") << m.strands << " strands, hash " << m.hash;
}

Made made(const StrandsPtr& strands) {
    Made m;
    if (!strands) {
        m.error = "nothing made";
        return m;
    }
    m.hash = everything(*strands);
    m.strands = strands->strandCount();
    return m;
}

void expectSameResults(const std::map<std::string, Made>& got, const std::map<std::string, Made>& want, const std::string& how) {
    ASSERT_EQ(got.size(), want.size());
    for (const auto& [name, m] : want) EXPECT_EQ(got.at(name), m) << name << ": " << how;
}

}  // namespace

#if GROOMLAB_HAS_TBB
#endif

// ---- The same bits on every machine ------------------------------------------------------------
// Every preset's strands against the bits recorded in tests/data/golden/PresetDigests.json, each
// part hashed on its own so a difference names what differs. The float math is compiled as written
// everywhere (CMakeLists.txt, "Determinism"); what still tells platforms apart is the C runtime's
// float sin/exp/pow... (portable_math.h). So the record names the math it was made with: a build
// with the same math (Windows' MSVC runtime, or GROOMLAB_PORTABLE_MATH on any platform) must make
// its bits exactly; another (Linux's glibc without it) reports how many presets match and fails (a stale build cache).
// GROOMLAB_UPDATE_GOLDEN=1 re-records the file (tools/dev/record_preset_looks.py does, with
// PresetLooks); GROOMLAB_DIGESTS_OUT=<file> writes this build's digests there, to compare by hand.

namespace {

using json = nlohmann::json;

std::string hex(std::uint64_t h) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(h));
    return text;
}

json digests(const StrandSet& s) {
    json channels = json::object();
    for (const std::string& name : s.strandChannelNames()) channels["strand." + name] = hex(hashArray(0, *s.strandChannel(name)));
    for (const std::string& name : s.pointChannelNames()) channels["point." + name] = hex(hashArray(0, *s.pointChannel(name)));
    std::vector<float> roots;
    roots.reserve(s.roots().size() * 3);
    for (const StrandRoot& r : s.roots()) {
        float face = 0.0f;
        static_assert(sizeof(face) == sizeof(r.face), "a face index fits a float's bits");
        std::memcpy(&face, &r.face, sizeof face);
        roots.insert(roots.end(), {face, r.u, r.v});
    }
    return {{"strands", s.strandCount()},
            {"points", s.pointCount()},
            {"everything", hex(everything(s))},
            {"ids", hex(hashArray(0, s.ids()))},
            {"roots", hex(hashArray(0, roots))},
            {"positions", hex(hashArray(0, s.positions()))},
            {"widths", hex(hashArray(0, s.widths()))},
            {"channels", channels}};
}

// The math this build evaluates transcendental functions with (portable_math.h).
std::string buildMath() {
#if GROOMLAB_PORTABLE_MATH
    return "portable";
#elif defined(_MSC_VER)
    return "msvc";
#elif defined(__GLIBC__)
    return "glibc";
#else
    return "other";
#endif
}

bool envSet(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}

// The parts of a preset's digests that differ from the recorded ones ("" when none).
std::string differingParts(const json& got, const json& want) {
    if (got == want) return {};
    std::string parts;
    for (const auto& [key, value] : want.items()) {
        if (key == "channels" && got.contains(key)) {
            for (const auto& [channel, digest] : value.items()) {
                if (!got[key].contains(channel) || got[key][channel] != digest) parts += " " + channel;
            }
        } else if (!got.contains(key) || got.at(key) != value) {
            parts += " " + key;
        }
    }
    return parts.empty() ? " (other channels)" : parts;
}

}  // namespace

TEST(Determinism, EveryFloatAGroomFileHoldsReadsBackBitForBit) {
    // The shortest text of a float read as a double and rounded to a float lands on the neighbour
    // for one magnitude (bits 15ae43fd: 7.038531e-26, found by trying every finite float); the
    // writer spells those longer. Its neighbours, zeros, the extremes and a few ordinary values too.
    const GroomFile& ref = reference().file;
    std::vector<Vec3f> points = ref.data.scalp->points();
    ASSERT_GE(points.size(), 4u);
    auto bitsOf = [](float f) {
        std::uint32_t b = 0;
        std::memcpy(&b, &f, 4);
        return b;
    };
    auto floatOf = [](std::uint32_t b) {
        float f = 0.0f;
        std::memcpy(&f, &b, 4);
        return f;
    };
    const std::vector<std::uint32_t> tricky = {0x15ae43fdu, 0x95ae43fdu, 0x15ae43fcu, 0x15ae43feu, 0x00000001u, 0x80000000u,
                                               0x7f7fffffu, 0xff7fffffu, 0x00800000u, 0x3f800001u, 0x4b7fffffu, 0x5f000000u};
    for (std::size_t i = 0; i < tricky.size(); ++i) {
        Vec3f& p = points[i % points.size()];
        (i % 3 == 0 ? p.x : i % 3 == 1 ? p.y : p.z) = floatOf(tricky[i]);
    }
    GroomFile file = ref;
    file.data.scalp = std::make_shared<ScalpMesh>(ref.data.scalp->withPoints(points));
    const GroomFile back = groomFileFromJson(groomFileToJson(file), ref.directory);
    ASSERT_TRUE(back.data.scalp);
    ASSERT_EQ(back.data.scalp->points().size(), points.size());
    for (std::size_t i = 0; i < tricky.size(); ++i) {
        const Vec3f& p = back.data.scalp->points()[i % points.size()];
        EXPECT_EQ(bitsOf(i % 3 == 0 ? p.x : i % 3 == 1 ? p.y : p.z), tricky[i]) << std::hex << tricky[i];
    }
}

}  // namespace groomlab
