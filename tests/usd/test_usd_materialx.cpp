// MaterialX: networks in USD stages (hand-written in fixtures/, and a referenced .mtlx), exportUsd's MaterialX
// networks, and standalone .mtlx documents through importMaterialX and exportMaterialX.

#include "UsdTestUtil.h"

#include "io/FileTranslators.h"

using namespace lila;
using namespace lila::scene;
using namespace lila::usdtest;

namespace
{
/** Materials covering what the networks carry: constants, every map kind, packing, DirectX normals, UV set 1, unlit. */
std::vector<std::string> buildMaterials()
{
    fresh();
    Graph& g = graph();
    const fs::path tex = tempDir() / "mtlx_tex";
    fs::create_directories(tex);
    for (const char* f : {"base.png", "n.png", "mro.png", "e.png", "m.png", "r.png", "sign.png"})
        std::ofstream(tex / f) << "png";
    auto path = [&](const char* f) { return (tex / f).generic_string(); };
    auto set = [&](Node* n, int attr, PlugValue v) { g.setValue({n->id(), attr, -1}, std::move(v)); };

    Node* brick = materials::create(g, "Brick");
    set(brick, material::kBaseColor, glm::dvec3(0.6, 0.25, 0.1));
    set(brick, material::kMetallic, 0.2);
    set(brick, material::kRoughness, 0.7);
    set(brick, material::kSpecular, 0.3);
    set(brick, material::kEmissiveColor, glm::dvec3(0.1, 0.0, 0.0));
    set(brick, material::kEmissiveIntensity, 2.0);

    Node* glass = materials::create(g, "Glass");
    set(glass, material::kBaseColorMap, path("base.png"));
    set(glass, material::kNormalMap, path("n.png"));
    set(glass, material::kOrmMap, path("mro.png"));
    set(glass, material::kPackedLayout, int64_t(material::PackedLayout::MRO));
    set(glass, material::kEmissiveMap, path("e.png"));
    set(glass, material::kEmissiveIntensity, 3.0);
    set(glass, material::kBlendMode, int64_t(material::BlendMode::Translucent));
    set(glass, material::kOpacity, 0.3);
    set(glass, material::kTwoSided, true);
    set(glass, material::kUvSet, int64_t(1));
    materials::setSampling(g, glass->id(), material::kBaseColorMap,
                           {material::TextureWrap::Clamp, material::TextureWrap::Black, material::TextureFilter::Nearest});
    materials::setSampling(g, glass->id(), material::kNormalMap, {material::TextureWrap::Mirror, material::TextureWrap::Repeat, {}});

    Node* rubber = materials::create(g, "Rubber");
    set(rubber, material::kMetallicMap, path("m.png"));
    set(rubber, material::kRoughnessMap, path("r.png"));
    set(rubber, material::kNormalMap, path("n.png"));
    set(rubber, material::kNormalFormat, int64_t(material::NormalFormat::OpenGL));
    set(rubber, material::kBlendMode, int64_t(material::BlendMode::Masked));
    set(rubber, material::kBaseColorMap, path("base.png"));
    set(rubber, material::kOpacityMaskClip, 0.4);

    Node* sign = materials::create(g, "Sign");
    set(sign, material::kShadingModel, int64_t(material::ShadingModel::Unlit));
    set(sign, material::kBaseColorMap, path("sign.png"));
    set(sign, material::kBlendMode, int64_t(material::BlendMode::Translucent));
    set(sign, material::kOpacity, 0.5);

    Node* flat = materials::create(g, "Flat");
    set(flat, material::kShadingModel, int64_t(material::ShadingModel::Unlit));
    set(flat, material::kBaseColor, glm::dvec3(0.2, 0.4, 0.8));

    const std::vector<std::string> names{"Brick", "Glass", "Rubber", "Sign", "Flat"};
    for (const std::string& n : names)
    {
        addMesh(n + "Mesh", geo::makeCube({.width = 10, .height = 10, .depth = 10}));
        run("assignMaterial", {{"args", {n, n + "Mesh"}}});
    }
    return names;
}
} // namespace

TEST_CASE("usd materialx: OpenPBR, Standard Surface, glTF PBR and referenced .mtlx networks map to Lila materials")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    json r = run("importUsd", {{"args", {fixture("mtlx_materials.usda").generic_string()}}, {"report", true}});
    INFO(r.dump(2));

    Node* painted = materials::find(graph(), "Painted");
    REQUIRE(painted);
    CHECK(painted->as<std::string>(material::kBaseColorMap).ends_with("tex/leaf_albedo.png"));
    CHECK(materials::sampling(*painted, material::kBaseColorMap).wrapU == material::TextureWrap::Clamp);
    CHECK(painted->as<std::string>(material::kOrmMap).ends_with("tex/leaf_orm.png"));
    CHECK(painted->as<int64_t>(material::kPackedLayout) == int64_t(material::PackedLayout::ORM));
    CHECK(painted->as<std::string>(material::kNormalMap).ends_with("tex/leaf_normal.png"));
    CHECK(painted->as<int64_t>(material::kNormalFormat) == int64_t(material::NormalFormat::OpenGL));
    CHECK(painted->as<int64_t>(material::kUvSet) == 1);
    CHECK(painted->as<double>(material::kSpecular) == doctest::Approx(0.5));
    CHECK(near(painted->as<glm::dvec3>(material::kEmissiveColor), {1, 0.5, 0}, 1e-6));
    CHECK(painted->as<double>(material::kEmissiveIntensity) == doctest::Approx(2.0));
    CHECK(listed(r["skipped"], "material Painted: coat_weight 0.3 has no Lila equivalent"));
    CHECK(listed(r["skipped"], "material Painted: specular_color is connected to a ND_noise2d_color3 node"));

    Node* chrome = materials::find(graph(), "Chrome");
    REQUIRE(chrome);
    CHECK(near(chrome->as<glm::dvec3>(material::kBaseColor), {0.125, 0, 0.375}, 1e-6));
    CHECK(chrome->as<double>(material::kMetallic) == doctest::Approx(1.0));
    CHECK(chrome->as<double>(material::kRoughness) == doctest::Approx(0.2));
    CHECK(chrome->as<int64_t>(material::kBlendMode) == int64_t(material::BlendMode::Translucent));
    CHECK(chrome->as<double>(material::kOpacity) == doctest::Approx(0.5));
    CHECK(listed(r["skipped"], "material Chrome: transmission 0.8"));

    Node* leaf = materials::find(graph(), "GltfLeaf");
    REQUIRE(leaf);
    CHECK(leaf->as<std::string>(material::kBaseColorMap).ends_with("tex/leaf_albedo.png"));
    CHECK(leaf->as<int64_t>(material::kBlendMode) == int64_t(material::BlendMode::Masked));
    CHECK(leaf->as<double>(material::kOpacityMaskClip) == doctest::Approx(0.3));
    CHECK(leaf->as<std::string>(material::kOrmMap).ends_with("tex/leaf_orm.png"));
    CHECK(leaf->as<int64_t>(material::kNormalFormat) == int64_t(material::NormalFormat::DirectX));
    CHECK(materials::sampling(*leaf, material::kNormalMap).wrapV == material::TextureWrap::Mirror);
    CHECK(listed(r["warnings"], "material GltfLeaf: the base color texture's tiling (2 2)"));
    CHECK(!listed(r["warnings"], "opacity comes from its own texture"));

    CHECK(listed(r["skipped"], "material Noisy: base_color comes from a ND_noise3d_color3 node"));
    CHECK(materials::find(graph(), "Noisy")->as<double>(material::kRoughness) == doctest::Approx(0.6));
    CHECK(listed(r["skipped"], "material Principled: its MaterialX surface ND_disney_principled has no Lila mapping"));

    // From the .mtlx the stage references, through usdMtlx.
    Node* copper = materials::find(graph(), "Copper");
    REQUIRE(copper);
    CHECK(near(copper->as<glm::dvec3>(material::kBaseColor), {0.95, 0.64, 0.54}, 1e-6));
    CHECK(copper->as<double>(material::kMetallic) == doctest::Approx(1.0));
    CHECK(copper->as<std::string>(material::kRoughnessMap).ends_with("tex/leaf_orm.png"));
    CHECK(copper->as<int64_t>(material::kUvSet) == 1);
    const material::TextureSampling rough = materials::sampling(*copper, material::kRoughnessMap);
    CHECK(rough.wrapU == material::TextureWrap::Clamp);
    CHECK(rough.filter == material::TextureFilter::Nearest);
    Node* velvet = materials::find(graph(), "Velvet");
    REQUIRE(velvet);
    CHECK(near(velvet->as<glm::dvec3>(material::kBaseColor), {0.4, 0.05, 0.1}, 1e-6));
    CHECK(velvet->as<double>(material::kRoughness) == doctest::Approx(0.6));
    CHECK(listed(r["skipped"], "material Velvet: sheen 0.8"));
    CHECK(materials::slotName(*outMesh("Pipe"), 0) == "Copper");
}

TEST_CASE("usd materialx: exportUsd writes OpenPBR or Standard Surface networks that import back")
{
    UpAxisScope axis(math::UpAxis::Z);
    for (const char* surface : {"openPBR", "standardSurface"})
        for (bool preview : {false, true})
        {
            INFO(surface << (preview ? " with" : " without") << " UsdPreviewSurface");
            const std::vector<std::string> names = buildMaterials();
            std::map<std::string, std::map<std::string, json>> before;
            for (const std::string& n : names)
                before[n] = materialValues(n);
            const fs::path file = tempDir() / "mtlx_out.usda";
            json out = run("exportUsd", {{"args", {file.generic_string()}},
                                         {"selectionOnly", false},
                                         {"materialX", surface},
                                         {"previewSurface", preview},
                                         {"report", true}});
            CHECK(out["warnings"].empty());
            const std::string text = readAll(file);
            CHECK(text.find(std::string(surface) == "openPBR" ? "ND_open_pbr_surface_surfaceshader" : "ND_standard_surface_surfaceshader") !=
                  std::string::npos);
            CHECK(text.find("ND_surface_unlit") != std::string::npos);
            CHECK(text.find("token outputs:mtlx:surface.connect") != std::string::npos);
            CHECK(text.find("string inputs:geomprop = \"st1\"") != std::string::npos);
            CHECK(text.find("colorSpace = \"srgb_texture\"") != std::string::npos);
            CHECK((text.find("UsdPreviewSurface") != std::string::npos) == preview);

            fresh();
            json r = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
            INFO(r.dump(2));
            CHECK(r["warnings"].empty());
            CHECK(r["skipped"].empty());
            for (const std::string& n : names)
                compareMaterial(n, before[n]);
        }
}

TEST_CASE("usd materialx: Lila's MaterialX networks read without Lila's settings, as other tools' would be")
{
    UpAxisScope axis(math::UpAxis::Z);
    buildMaterials();
    const fs::path file = tempDir() / "mtlx_plain.usda";
    run("exportUsd", {{"args", {file.generic_string()}}, {"selectionOnly", false}, {"materialX", "openPBR"}, {"previewSurface", false}});
    std::string text = readAll(file);
    // Drop the customData Lila reads its own settings from.
    for (size_t at; (at = text.find("customData")) != std::string::npos;)
    {
        size_t open = text.find('{', at), depth = 0, end = open;
        for (; end < text.size(); ++end)
        {
            depth += text[end] == '{';
            if (text[end] == '}' && --depth == 0)
                break;
        }
        text.erase(at, end + 1 - at);
    }
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;

    fresh();
    json r = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
    INFO(r.dump(2));
    Node& brick = node("Brick");
    CHECK(near(brick.as<glm::dvec3>(material::kBaseColor), {0.6, 0.25, 0.1}, 1e-6));
    CHECK(brick.as<double>(material::kRoughness) == doctest::Approx(0.7));
    CHECK(brick.as<double>(material::kSpecular) == doctest::Approx(0.3).epsilon(1e-4));
    CHECK(brick.as<double>(material::kEmissiveIntensity) == doctest::Approx(2.0));
    Node& glass = node("Glass");
    CHECK(glass.as<int64_t>(material::kPackedLayout) == int64_t(material::PackedLayout::MRO));
    CHECK(glass.as<int64_t>(material::kNormalFormat) == int64_t(material::NormalFormat::DirectX));
    CHECK(glass.as<int64_t>(material::kUvSet) == 1);
    CHECK(glass.as<int64_t>(material::kBlendMode) == int64_t(material::BlendMode::Translucent));
    CHECK(glass.as<double>(material::kOpacity) == doctest::Approx(0.3));
    CHECK(glass.as<double>(material::kEmissiveIntensity) == doctest::Approx(3.0));
    CHECK(materials::sampling(glass, material::kBaseColorMap).wrapV == material::TextureWrap::Black);
    CHECK(node("Rubber").as<std::string>(material::kMetallicMap).ends_with("m.png"));
    CHECK(node("Rubber").as<int64_t>(material::kNormalFormat) == int64_t(material::NormalFormat::OpenGL));
    CHECK(node("Sign").as<int64_t>(material::kShadingModel) == int64_t(material::ShadingModel::Unlit));
    CHECK(node("Sign").as<double>(material::kOpacity) == doctest::Approx(0.5));
    CHECK(near(node("Flat").as<glm::dvec3>(material::kBaseColor), {0.2, 0.4, 0.8}, 1e-6));
}

TEST_CASE("usd materialx: an occlusion map MaterialX can't hold is reported when it goes out alone")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    std::ofstream(tempDir() / "ao.png") << "png";
    Node* m = materials::create(graph(), "Stone");
    graph().setValue({m->id(), material::kAmbientOcclusionMap, -1}, (tempDir() / "ao.png").generic_string());
    addMesh("Rock", geo::makeCube({}));
    run("assignMaterial", {{"args", {"Stone", "Rock"}}});
    const fs::path file = tempDir() / "mtlx_ao.usda";
    json out = run("exportUsd", {{"args", {file.generic_string(), "Rock"}}, {"materialX", "openPBR"}, {"previewSurface", false}, {"report", true}});
    CHECK(listed(out["warnings"], "material Stone: open_pbr_surface has no occlusion input"));
    out = run("exportUsd", {{"args", {file.generic_string(), "Rock"}}, {"materialX", "openPBR"}, {"report", true}});
    CHECK(out["warnings"].empty());
    CHECK_THROWS(run("exportUsd", {{"args", {file.generic_string(), "Rock"}}, {"previewSurface", false}}));
}

TEST_CASE("usd materialx: .mtlx documents export and import through the MaterialX library")
{
    UpAxisScope axis(math::UpAxis::Z);
    for (const char* surface : {"openPBR", "standardSurface"})
    {
        INFO(surface);
        const std::vector<std::string> names = buildMaterials();
        std::map<std::string, std::map<std::string, json>> before;
        for (const std::string& n : names)
            before[n] = materialValues(n);
        const fs::path file = tempDir() / "mtlx_doc" / "materials.mtlx";
        json out = run("exportMaterialX", {{"args", {file.generic_string()}}, {"selectionOnly", false}, {"surface", surface}, {"report", true}});
        INFO(out.dump(2));
        CHECK(out["warnings"].empty());
        CHECK(out["materials"].size() == names.size());
        const std::string text = readAll(file);
        CHECK(text.find("<surfacematerial name=\"Glass\"") != std::string::npos);
        CHECK(text.find("nodegraph name=\"NG_Glass\"") != std::string::npos);
        CHECK(text.find("<texcoord") != std::string::npos);
        CHECK(text.find("../mtlx_tex/base.png\"") != std::string::npos);

        fresh();
        json r = run("importMaterialX", {{"args", {file.generic_string()}}, {"report", true}});
        INFO(r.dump(2));
        CHECK(r["created"].size() == names.size());
        CHECK(r["warnings"].empty());
        CHECK(r["skipped"].empty());
        for (const std::string& n : names)
            compareMaterial(n, before[n]);

        // Into a scene that has them already: left as they are.
        r = run("importMaterialX", {{"args", {file.generic_string()}}, {"report", true}});
        CHECK(r["created"].empty());
        CHECK(listed(r["warnings"], "material Brick is already in the scene"));
    }

    // A document from elsewhere, with what Lila can't keep reported; and the translator File > Import uses.
    fresh();
    json r = run("importMaterialX", {{"args", {fixture("mtlx_library.mtlx").generic_string()}}, {"report", true}});
    INFO(r.dump(2));
    CHECK(r["created"] == json({"Copper", "Velvet"}));
    CHECK(listed(r["skipped"], "material Velvet: sheen 0.8"));
    CHECK(node("Copper").as<std::string>(material::kRoughnessMap).ends_with("tex/leaf_orm.png"));
    fresh();
    io::translators::run(io::translators::Op::Import, fixture("mtlx_library.mtlx"));
    CHECK(materials::find(graph(), "Velvet"));
}
